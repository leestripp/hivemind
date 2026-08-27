# OmniNet Bug Report — HTTP Server Connection-Thread Leak

**Date:** 2026-08-27
**Component:** `OmniNet::HTTP::Server` (v0.9.7)
**Severity:** High — unbounded memory growth on any public-facing server
**Found by:** hive-server RSS investigation (droplet grew ~60 KB/s idle; reproducible
locally at ~8 MB/s under modest load)

---

> **RESOLVED upstream 2026-08-27 (OmniNet 1.0.0, commit 00ceabc).** The fix
> implements Option A verbatim (completion-flag + reaper in
> `reapFinishedConnectionThreadsLocked()`), and upstream added the T1/T2
> regression tests below (`tests/test_sync_http_server.cpp`, verified to fail
> pre-fix). HiveMind verified against 1.0.0: 1000 fresh connections →
> VmSize +82 MB / RSS +0.3 MB (was +8.2 GB / +20 MB). To avoid stale
> vendored checkouts, HiveMind's root CMakeLists now fetches+fast-forwards
> the FetchContent checkout of `main` at configure time
> (`OMNINET_AUTO_UPDATE`, default ON, skipped if the checkout is dirty).

## 1. Summary

`HttpServer::handleConnection()` spawns a **joinable** `std::thread` for every
accepted TCP connection and stores it in `m_connectionThreads`. Those threads are
**only ever joined in `Server::stop()`**.

A joinable thread that has finished executing is a *zombie thread*: glibc keeps its
entire stack (default 8 MB) and thread control block mapped until `pthread_join()`
is called on it. Nothing calls join during normal operation, so:

> **Every fresh TCP connection permanently leaks ~8 MB of address space plus the
> resident pages the handler touched (~20 KB).**

## 2. Location

`src/http/HttpServer.cpp`:

```cpp
void Server::handleConnection(std::unique_ptr<Socket> socket) {
    // ...
    // Spawn a new thread to handle this connection. Track it so stop()
    // can join it and avoid a use-after-free race with the Server destructor.
    std::thread connThread([this, socket = std::move(socket)]() mutable {
        // ... Connection conn(std::move(socket)); conn.processRequests(); ...
    });

    {
        std::lock_guard<std::mutex> lock(m_connectionThreadsMutex);
        m_connectionThreads.push_back(std::move(connThread));   // ← never reaped
    }
}
```

`m_connectionThreads` (`std::vector<std::thread>`, HttpServer.h:245) is only drained
in `stop()`:

```cpp
// Join any active connection handler threads. They were previously
// detached, which left them racing with the Server destructor.
{
    std::lock_guard<std::mutex> lock(m_connectionThreadsMutex);
    for (auto& t : m_connectionThreads) {
        if (t.joinable()) t.join();
    }
    m_connectionThreads.clear();
}
```

The comment documents the *previous* bug (detached threads racing the destructor).
The fix introduced the *current* bug: tracked-but-never-reaped joinable threads.

## 3. Measured Evidence (hive-server, Ubuntu 26.04, glibc 2.4x)

| Test | Result |
|------|--------|
| 1000 sequential fresh-connection GETs | VmSize **+8.2 GB** (~8 MB/conn), RSS +20 MB (~20 KB/conn), live `Threads:` count constant |
| 2000 requests over **one keep-alive connection** | **zero growth** — one thread serves all requests |
| 4-parallel fresh-connection hammer | RSS ~+8 MB/s, VmSize ~+3 GB/s, never returns |
| Single contiguous anon mapping in `/proc/pid/maps` | grew to **173 GB** — merged zombie thread stacks (grows *downward* as new stacks mmap below) |
| valgrind memcheck, 3,678 requests, `--show-leak-kinds=all` | **0 bytes lost, 56 bytes still reachable** — valgrind manages pthread stacks in its own pool, so this leak is *invisible* to memcheck |
| jemalloc (`LD_PRELOAD`) | identical growth — not a malloc issue |
| glibc `mallopt` tuning (`M_ARENA_MAX`, `M_MMAP_THRESHOLD`, `M_TRIM_THRESHOLD`) | no effect — not an arena/fragmentation issue |

Production correlation: the HiveMind droplet's RSS climbed ~60 KB/s with no active
work — the GTK client polls the API every 1.5 s with a fresh connection per request
(~2.7 conn/s × ~20 KB RSS + occasional bigger handlers).

## 4. Reproduction

```bash
# any OmniNet HTTP server on :8421
BASE=$(awk '/VmSize/{print $2}' /proc/$SRV_PID/status)
for i in $(seq 1 1000); do curl -s -o /dev/null http://127.0.0.1:8421/any/route; done
NOW=$(awk '/VmSize/{print $2}' /proc/$SRV_PID/status)
# (NOW-BASE) ≈ 8,000,000 KB  → ~8 MB per connection = leaked thread stacks
```

Also visible without load tooling: the `m_connectionThreads` vector grows by one
element per connection for the lifetime of the process (small but unbounded, and
each element pins a zombie stack).

## 5. Why Valgrind Didn't Catch It

Memcheck intercepts `malloc`/`new` but valgrind replaces pthread stack management
with its own pool; exited-unjoined guest threads do not accumulate guest-visible
stacks under valgrind. Standard heap leak tooling is blind to this class of bug.
Detection requires OS-level observation: `/proc/<pid>/status` VmSize/VmRSS deltas,
`/proc/<pid>/maps` region growth, or `pthread` bookkeeping.

## 6. Patch Suggestions

### Option A — Self-flagging threads + periodic reaper (minimal, recommended)

`std::thread` has no "finished" query, so a reaper needs a completion flag per
thread. Keep the vector, but store a small record; connection threads flag
themselves done on exit; the accept loop joins-and-erases finished records every
N accepts (or when the vector exceeds a watermark):

```cpp
// HttpServer.h
struct ConnectionThread {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
};
std::vector<ConnectionThread> m_connectionThreads;   // was std::vector<std::thread>
std::mutex m_connectionThreadsMutex;
```

```cpp
// HttpServer.cpp — handleConnection()
auto done = std::make_shared<std::atomic<bool>>(false);
std::thread connThread([this, socket = std::move(socket), done]() mutable {
    struct FlagGuard {           // flag even on exception
        std::shared_ptr<std::atomic<bool>> d;
        ~FlagGuard() { d->store(true, std::memory_order_release); }
    } guard{done};
    try {
        Connection conn(std::move(socket));
        conn.processRequests();
    } catch (...) { /* existing logging */ }
});
{
    std::lock_guard<std::mutex> lock(m_connectionThreadsMutex);
    m_connectionThreads.push_back({std::move(connThread), std::move(done)});
    reapFinishedLocked();        // cheap: only joins flagged (exited) threads
}

void Server::reapFinishedLocked() {   // caller holds m_connectionThreadsMutex
    auto it = m_connectionThreads.begin();
    while (it != m_connectionThreads.end()) {
        if (it->done->load(std::memory_order_acquire)) {
            it->thread.join();   // returns immediately — thread already exited
            it = m_connectionThreads.erase(it);
        } else ++it;
    }
}
```

`stop()` is unchanged (joins whatever remains). Shutdown semantics are preserved:
live connection threads are still tracked and joined; only *finished* ones are
reaped early. Cost: one `shared_ptr<atomic<bool>>` per connection and an O(n)
scan per accept (n = live connections, small).

### Option B — Detach + active-connection wait-group (simplest)

Revert to `detach()` (fixing the leak at the source: detached threads free their
stack at exit) and fix the original destructor race with a counter instead of joins:

```cpp
// HttpServer.h
std::atomic<int64_t> m_activeConnections{0};
std::mutex m_drainMutex;
std::condition_variable m_drainCv;     // signalled when count reaches 0
```

```cpp
// handleConnection()
m_activeConnections.fetch_add(1, std::memory_order_relaxed);
std::thread([this, socket = std::move(socket)]() mutable {
    try { Connection conn(std::move(socket)); conn.processRequests(); }
    catch (...) { /* existing logging */ }
    if (m_activeConnections.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> lk(m_drainMutex);
        m_drainCv.notify_all();
    }
}).detach();

// stop() — after the accept loop has exited (no new connections):
std::unique_lock<std::mutex> lk(m_drainMutex);
m_drainCv.wait(lk, [this] { return m_activeConnections.load() == 0; });
```

This is arguably the cleaner design: the destructor race the joinable-vector was
meant to fix is fully covered by draining on the counter, and no per-connection
bookkeeping structures accumulate at all. Consider also a **shutdown grace period**
(e.g. wait up to N seconds, then force-close live sockets via the existing
`m_activeStreams`-style mechanism) so a stuck connection can't hang `stop()`
forever.

### Option C — Real worker pool (larger redesign, optional)

`Server::start()` already spawns `m_workers` ("worker threads") — but
`workerLoop()` is a 100 ms spin that does nothing; connections bypass it entirely.
A follow-up redesign could hand accepted sockets to the worker pool (bounded thread
count, no per-connection stacks at all). Not required to fix this bug; Options A/B
are sufficient.

### Recommendation

**Option A** if you want the smallest behavioural delta. **Option B** if you're
comfortable re-resolving the shutdown race with a wait-group (it is the standard
pattern and deletes more code than it adds).

## 7. Unit / Regression Test Ideas

All tests are Linux-friendly and runnable in CI without external load tools.

### T1 — VmSize growth bound (the core regression test)

```cpp
TEST(HttpServer, NoAddressSpaceGrowthPerConnection) {
    auto server = /* start on ephemeral port, one trivial route */;
    long vms_before = readVmSizeKB();          // parse /proc/self/status
    for (int i = 0; i < 500; ++i) {
        auto c = HttpClient::connect("127.0.0.1", port);   // fresh connection each time
        c.get("/ping");
        c.close();                                          // ensure connection ends
    }
    long vms_after = readVmSizeKB();
    // Pre-fix this grows ~4 GB (500 × 8 MB). Post-fix: bounded by heap noise.
    EXPECT_LT(vms_after - vms_before, 64 * 1024);  // < 64 MB
}
```

Also assert RSS growth `< 32 MB` (pre-fix: ~10 MB per 500 conns — keep the
threshold generous to avoid flakes on small-footprint handlers).

### T2 — Keep-alive control (should pass before AND after the fix)

One connection, 500 sequential requests, assert near-zero VmSize/RSS delta.
Guards against a future regression where *requests* (not connections) leak.

### T3 — Bookkeeping bound (white-box)

If `Stats` (or a debug accessor) exposed `connectionThreadsTracked` / a gauge of
`m_connectionThreads.size()`: after N connections have fully completed, the tracked
count must return to ~the number of *live* connections (≈0), not N. This fails
pre-fix deterministically and needs no `/proc` parsing.

### T4 — Shutdown-join regression (guards the original bug the joinable-vector fixed)

Churn 200 short-lived connections, call `stop()` while a few slow/streaming
requests are still in flight, assert: `stop()` returns within a timeout, no
`std::terminate`, no use-after-free under ASan. This is the behaviour any fix must
preserve.

### T5 — Concurrent churn stress (catches reaper races in Option A)

32 threads × 100 connections each against the server, then T1's VmSize assertion.
Under Option A this exercises the reaper's erase-while-scanning path; under
Option B it exercises the counter's inc/dec symmetry (final count must be 0).

### T6 — valgrind/ASan job (CI hygiene)

Not for this bug (memcheck is blind to it — see §5), but keep a helgrind/TSan run
over T5: Option A's flag-then-join and Option B's counter both have obvious
happens-before requirements worth pinning down.

## 8. HiveMind-Side Notes (non-OmniNet)

- hive-client's `ApiClient` should reuse one keep-alive connection per poll cycle
  instead of a fresh connection per request — collapses production growth to ~zero
  regardless of the OmniNet fix. (Browser clients already pool via `fetch()`.)
- The exploratory `mallopt()` tuning added to hive-server during the investigation
  was reverted: irrelevant to this bug.
- hive-server's new `/api/system` endpoint reports its own RSS, which makes this
  class of regression observable in production going forward.

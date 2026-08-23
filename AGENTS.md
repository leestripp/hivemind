# AGENTS.md — HiveMind 2.0

> Quick-start reference for AI coding agents. Read this before touching code.
> Deeper rationale: `docs/design.md`. Roadmap & phase status: `Plan.md`.

---

## 1. What this is

HiveMind 2.0 is a **self-hosted team of always-on AI agents for Linux**
(Ubuntu 26.04 target; no Windows/macOS). Persistent named teammates
(`daedalus::Agent`) you message in threads; they run commands in their own
rootless podman containers as user `hive` (passwordless sudo), delegate to
each other, wait for human approval when gated, and fire scheduled routines.
The 1.x node-graph workflow engine (web canvas) is kept intact.

| Property | Value |
|----------|-------|
| Language | **C++23** |
| Build | **CMake 3.28+** superbuild — `./hivemindctl build` |
| Server | `hive-server/` → `build/hive-server/hive-server` (OmniNet, port 8421) |
| Client | `hive-client/` → `build/hive-client/hive-client` (GTK4/gtkmm4) |
| Agent engine | **daedalus** `libdaedalus.a` (FetchContent, tag `0.9.6`) |
| HTTP | **OmniNet** (FetchContent, `main`) |
| Tests | `ctest --test-dir build` or `./hivemindctl test` (built `-UNDEBUG`) |

## 2. Hard rules (learned the expensive way)

1. **Never patch OmniNet or daedalus.** Consume public APIs as-is; bugs are
   in our code until proven otherwise.
2. **No LLM settings in HiveMind.** Models/providers/credentials live ONLY in
   the daedalus SettingsStore (`~/.local/share/daedalus/daedalus.db`).
3. **Agents never execute on the host** (except explicit
   `workspace_mode:"host"` escape hatch). Commands go through
   `container_exec` → podman.
4. **daedalus history persists ONLY via `Agent::shutdown()`** — `~Agent`
   does not save, and turn-loop mutations bypass auto-flush. Server signal
   path calls `AgentRegistry::shutdown_all()`. Never `kill -9` the server
   when you care about history.
5. **Serialize all daedalus Agent lifecycles** with `agent_exec_mutex()`
   (recursive — nested delegation turns are same-thread). daedalus 0.9.6
   shares global state; parallel agents crash it.
6. **OmniNet `pathParam(i)` is unreliable for multi-param routes** — parse
   `req.path()` with a local `path_segments()` helper (see any *Routes.cpp).
7. **Register API routes BEFORE `register_static_routes`** (first-match-wins;
   the static catch-all is last).
8. **Disabled tool names hide host tools too** — a replacement for a
   built-in tool needs a DISTINCT name (that's why `container_exec` exists).
9. **Kill dev servers with `pgrep -x hive-server`**, never a `-f` pattern
   that matches your own shell's command line.
10. Run the server from the **repo root** (web/dist and the Containerfile
    are resolved relative to cwd).
11. **Ownership rule (no refcount band-aids):** the AgentRegistry is the
    SOLE owner of AgentHosts (`unique_ptr`). Hosts are never destroyed
    mid-process — `evict()` marks dead, saves history (shutdown()), and
    retires to the graveyard; destruction happens once, in
    `shutdown_all()`, after `ChatService::quiesce()` has drained all turns.
    References from `get_or_create()` are valid for the process lifetime.
    Do NOT reintroduce shared_ptr here.

## 3. Layout map

```
hive-server/src/            GraphRunner, LoopEngine, stores (Graph/Agent/
                            Thread/Approval/Routine), AgentHost (+Registry,
                            ChatService), ContainerManager, executors/
hive-server/include/hivemind/routes/   REST registrars (h+cpp side by side)
hive-server/data/           Containerfile.hive-agent (+browser variant),
                            hive-server.service
hive-client/src/            HiveApplication, HiveWindow, ApiClient
tests/                      one binary per store/subsystem
```

State at runtime (`~/.local/share/hivemind/`): `hivemind.db` (1.x graphs),
`agents.db`, `threads.db`, `approvals.db`, `routines.db`, `agents/<id>/`
(per-agent daedalus dir + container home).

## 4. Common tasks

```bash
./hivemindctl build && ./hivemindctl start   # build + run server :8421
./hivemindctl test                           # 9-test suite
./hivemindctl client                         # GTK client
./hivemindctl logs                           # follow server log
```

**Add a store:** copy `AgentStore` (own .db, fresh sqlite conn per op, path
ctor for tests). **Add routes:** `routes/FooRoutes.{h,cpp}` + register in
`main.cpp` before static routes. **Add an agent tool:** subclass
`daedalus::BaseTool`, register in `AgentHost::ensure_agent()` with a unique
name. **Add a test:** `tests/test_foo.cpp` + `hivemind_test(test_foo)`.

## 5. Environment notes (this dev sandbox)

- podman is installed but **containers cannot run here** (nested userns) —
  `test_container_manager` skips its live section; run it on a real box.
- No display: smoke-launch the client with `xvfb-run -a`.
- Live test model: **kimi-for-coding** (Kimi K2.7) in the SettingsStore.

## 6. Current status

Phases 0–5 + 7 done, Phase 9 token auth done (see Plan.md). Open: Phase 6
browser tools (blocked on sandbox), Phase 8 remainder (remote workers,
Flatpak, `/ws/runs` fix, perf pass), Phase 9 remainder (TLS, CORS
allowlist, deploy doc).

**Auth:** all `/api/*` calls need `Authorization: Bearer <key>`; the master
key prints once at first start (`hivemindctl logs`). curl examples need
`-H "Authorization: Bearer $KEY"`.

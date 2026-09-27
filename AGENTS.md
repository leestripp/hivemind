<!-- monkeybyte:type: cpp -->

# AGENTS.md — Guidance for AI Coding Agents

This file tells AI coding agents (and humans) how to work effectively in
the HiveMind repository. Read it before making changes.

---

## Project Overview

**HiveMind** is a self-hosted team of always-on AI agents for Linux.
Users create named, persistent agent "teammates," chat with them in
threads, and hand off real work. Agents run inside rootless **podman**
containers (with passwordless sudo as user `hive`), remember everything,
share files, and park commands in an approval queue when
`require_approval` is set.

**Architecture (see `docs/design.md`):**

- **The brain lives where the work is** — agents (hive-brain) run *inside*
  containers on worker machines; skills, MCP servers, shell access and
  files are local to the agent.
- **The server is a comms hub, not a host** — it routes messages between
  clients, workers and agents. It holds **no LLM credentials** and runs
  no workloads.
- **Outbound-only workers** — workers dial the hub over a single
  WebSocket; no inbound ports/DNS/TLS needed on rigs.
- **File locality** — artifacts stay on the machine that produced them.
- **Linux-only** — podman (rootless), systemd --user, GTK4/gtkmm4.

Current release line: **0.9.x** (latest tag `0.9.6`; distributed workers milestone was 0.5.0).

## Tech Stack

| Component | Tech |
|---|---|
| Language | **C++23** (required), C |
| Build | **CMake ≥ 3.28**, Ninja generator, `enable_testing()` + ctest |
| HTTP/WebSocket | **OmniNet** (first-party, fetched from gitlab.com/leestripp/omninet) — the *only* HTTP stack; **no libcurl** |
| Agent engine | **hive-agent** (in-tree at `hive-agent/src/` — merged into this repo 2026-09-07; builds as part of the main build) |
| Persistence | **SQLite3** (raw C API via `DbUtil.h` helpers) |
| JSON | **nlohmann_json** |
| Native client | **GTK4 / gtkmm-4.0** (+ optional WebKitGTK 6.0 for embedded noVNC) |
| Web dashboard | Vanilla JS + CSS in `web/dist/` (no framework, no bundler) |
| Agent sandbox | Rootless **podman**; on-demand desktop (Xvfb + x11vnc + noVNC + Chromium) per container — starts on demand, auto-sleeps when viewerless |
| Crypto/misc | OpenSSL, libpng, poppler-cpp (hive-brain deps) |

## Build & Test Commands

System packages (Ubuntu 26.04):

```bash
sudo apt install -y cmake g++ nlohmann-json3-dev \
    sqlite3 libsqlite3-dev libssl-dev libpng-dev libpoppler-cpp-dev \
    libgtkmm-4.0-dev podman   # no libcurl — OmniNet is the only HTTP stack
```

Configure & build (CMake auto-fetches hive-brain + OmniNet via FetchContent):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j"$(nproc)"
```

Useful configure options:

- `-DHIVEMIND_BUILD_SERVER=OFF` / `-DHIVEMIND_BUILD_CLIENT=OFF` — component toggles
- `-DOMNINET_LOCAL=/path/to/omninet` — use a local OmniNet checkout instead of fetching
- `-DOMNINET_AUTO_UPDATE=OFF` — stop configure-time `git fetch + reset --hard origin/main` of the vendored OmniNet
- Clang toolchain: `cmake -S . -B build-clang -DCMAKE_TOOLCHAIN_FILE=cmake/clang.cmake`

Tests (server-side only; skipped when `HIVEMIND_BUILD_SERVER=OFF`):

```bash
cmake --build build        # tests link hivemind_core
ctest --test-dir build --output-on-failure
# or a single test:
./build/hive-server/tests/test_agent_store
```

Lint (requires `compile_commands.json`, i.e. `CMAKE_EXPORT_COMPILE_COMMANDS=ON`):

```bash
cmake --build build --target lint       # dry run → lint-results.yaml
cmake --build build --target lint-fix   # auto-fix
```

Install: `cmake --install build` (default prefix `~/.local`).

Runtime: server API on **:8421**, worker WebSocket channel on **:8423**
(MCP servers run in-container — there is no hub-side bridge).
Enroll a worker with
`hive-server enroll-worker <name>`; connect with
`hive-worker --server wss://<server> --key hmwk_… --name <name>`
(TLS via the nginx proxy at `/api/workers/channel`; a co-hosted worker
on the hub machine uses plain `ws://127.0.0.1:8423` — loopback, no TLS
needed).

## Project Structure

```
CMakeLists.txt        Superbuild: deps (FetchContent), options, lint targets
cmake/clang.cmake     Clang toolchain file (stricter warnings)
hive-server/          C++23 + OmniNet hub: REST API, threads, approvals,
                      routines, worker channel, web UI (serves web/dist)
  include/hivemind/   Public headers + route implementations
    routes/           One file pair per API area (RosterRoutes, ThreadRoutes…)
  src/                Core: *Store classes (SQLite), ChatService, HubRouter,
                      WorkerChannel/WorkerRouter, ContainerManager, main.cpp
  data/               Containerfiles, screen scripts, seed /workspace content
  deploy/             systemd units, backup timer, remote deploy scripts
hive-worker/          Remote executor daemon (fleet manager). Links OmniNet
                      ONLY — no hive-brain, no LLM keys, no roster DB.
hive-agent/           In-container brain: thin hive-brain::Agent host speaking
                      stdio NDJSON with hive-worker. No HTTP.
hive-client/          GTK4/gtkmm4 native client (roster, threads, approvals,
                      screen view). Icons/avatars bundled via GResource.
*/tests/              per-app ctest suites — one binary per area, assert()-based
web/dist/             Web dashboard (vanilla JS pages under js/pages/)
docs/                 design.md, api.md, hive-worker.md, deploy.md, …
resources/            banner/icon images
```

Key architectural mapping:

- `hivemind_core` static lib (everything except `main.cpp`) is shared by
  the server binary **and** the test suite — add new core sources to
  `CORE_SOURCES` / `ROUTE_SOURCES` in `hive-server/CMakeLists.txt`.
- Server is a pure hub: OmniNet + SQLite only. hive-brain linkage was
  deliberately removed from the server (Phase C).
- Hub ↔ worker: JSON text frames over WS. Worker ↔ agent: stdio NDJSON
  (task/delta/message/approval/image events).

## Code Style & Conventions

- **C++23**, `-Wall -Wextra` everywhere; the clang toolchain adds
  `-Wpedantic -Wconversion -Wshadow -Wnull-dereference -Wdouble-promotion -Wformat=2`.
- **4-space indent**, snake_case functions/variables, PascalCase types
  (`AgentStore`, `RosterAgent`), trailing `_` for members (`db_path_`),
  `kCamelCase` for constants (`kSelectCols`).
- Header banner comments (`// ====…`) at the top of files referencing the
  relevant doc (e.g. "docs/hive-worker.md").
- Headers use `#pragma once`; Doxygen-style `/** … */` class docs and
  `///` member docs.
- **SQLite pattern**: fresh connection per operation (thread-safety),
  shared helpers in `hivemind/DbUtil.h` (`exec_sql`, `bind_text`,
  `col_text`, `uuid_v4`, `iso_now`). DB files live under
  `~/.local/share/hivemind/`.
- **Errors**: stores throw `std::invalid_argument` / `std::runtime_error`;
  routes translate to JSON errors via `hivemind::routes::send_error`.
- **Routing gotcha (important)**: OmniNet `pathParam(i)` is unreliable
  for multi-param routes — parse positional segments from `req.path()`
  with `hivemind::routes::path_segments` instead (see RosterRoutes.cpp).
- Register API routes **before** the static catch-all (first-match-wins);
  the web UI static handler must be last (see `src/main.cpp`).
- JSON via `using json = nlohmann::json;` in route files.
- Tests: plain `main()` + `assert()` (no framework), temp dirs under
  `fs::temp_directory_path()`, cleaned on start. Tests are compiled with
  `-UNDEBUG` so asserts stay live in Release — do not rely on NDEBUG.
- New tests: add `hivemind_test(test_name)` to `hive-server/tests/CMakeLists.txt`
  (worker/agent apps use `hive_worker_test` / `hive_agent_test` in their own tests/ dirs).

## Security Rules (do not regress)

- Bearer API keys are SHA-256 hashed at rest; master key shown once.
- Server binds localhost-only; TLS is terminated by nginx/Caddy in front.
- Worker keys (`hmwk_…`) are shown once at enrollment, hashed in
  `workers.db`; workers authenticate over `wss://`.
- LLM credentials are **worker-owned** (`models.json` + `secrets.json`
  mode 0600) and injected into agents as spawn env. **Never** store keys
  on the hub. Model/secret management routes
  (`/api/workers/:name/models|secrets`) are **relay-only**: they forward
  over the worker channel and persist nothing hub-side; keep them
  admin-scoped in the auth middleware. Agents run hive-brain with
  `explicit_model_wins` — models.json is the single registry of record;
  the per-agent hive-brain settings store must never shadow it.
- Workspace writes are path-confined — `send_file` destinations must not
  escape the company drive.

## Documentation Map

| File | Contents |
|---|---|
| `docs/design.md` | Architecture deep-dive (read first for big changes) |
| `docs/api.md` | REST API reference |
| `docs/hive-worker.md` | Worker enrollment, channel protocol, exec/screen ops |
| `docs/deploy.md` | VPS deployment, systemd, TLS, lockdown |
| `docs/containers.md` | Podman sandboxing |
| `docs/computer-use.md` | Live screen share (Xvfb/noVNC) |
| `docs/skills.md` | Global skill library design |
| `docs/client.md` / `docs/web.md` | GTK client / web dashboard guides |

## Practical Tips for Agents

1. **Build before and after** any change: `cmake --build build -j$(nproc)`.
   The superbuild fetches dependencies on first configure — expect that
   to take a while and require network access.
2. **Run the full ctest suite** after touching any `*Store` or routes
   code — route tests (`test_roster_routes`, `test_screen_routes`) catch
   path-parsing regressions.
3. Keep the **hub dumb**: new compute belongs on the worker/agent side;
   the server only routes and persists comms state.
4. Web UI changes are plain files in `web/dist/` — no build step; bump
   the `?v=` cache-buster query in `index.html` when editing CSS/JS.
5. Don't reintroduce libcurl or a second HTTP stack; use OmniNet.
6. `.hive-agent/` and `build/` are gitignored local state — never commit
   or delete them as "cleanup."
7. **popen pipes must always be drained** (`podman_simple` pattern) — an
   undrained pipe SIGPIPEs the child (rc 141) once it writes.
8. In agent containers, `pgrep` matches ZOMBIES (PID 1 `sleep` never
   reaps) — check socket state in `/proc/net/tcp` for liveness instead.
9. SQLite per-op connections (`open_conn()` pattern) MUST be closed
   (`sqlite3_close`) — a per-row helper in a LIST route multiplies the
   leak by rows × poll rate; an unclosed connection exhausted the
   hub's 1024-fd limit overnight and 500'd every authed request
   (EMFILE reads as "unable to open database file"). Check fd count
   (`ls /proc/<pid>/fd | wc -l`) before/after repeated route calls.
10. GTK theme on Ubuntu GNOME: the Appearance panel flips `gtk-theme`
   ('Yaru'↔'Yaru-dark') and leaves `color-scheme='default'` — read BOTH
   keys (see `HiveWindow::follow_system_theme`).

## Messaging (Phase 66)

Agent-to-agent messaging is a DURABLE queue (`deliveries.db`,
DeliveryStore): wakes survive hub restarts, gate refusals are recorded
(`dropped` + reason), and duplicates are suppressed by (target, source
message) idempotency. `message_agent` returns a `delivery_id`;
`message_status` reports `queued`/`dispatched`/`done`/`error`/`dropped`/
`expired` plus read receipts. Diagnose message loss with
`GET /api/deliveries?agent=&state=` before touching code. When changing
fan-in/relay logic, run test_agent_relay + test_delivery_store — the
single wake path (relay suppresses pair fan-in) is load-bearing.

Read state (66c) is per-principal high-water marks in
`thread_reads` (deliveries.db). A delivery's read receipt consults
ONLY the target agent's mark (`delivery_is_read`) — never let any
other principal's view advance an agent's mark. The human user has a
reserved `"user"` row: `POST /api/threads/:id/read` advances only it
(the GTK client fires it debounced on view), and `GET /api/threads`
carries `unread` = non-user messages newer than that mark — the
client's badge count, persisted across restarts (the old in-memory
`last_seen_activity_` badge died on every launch; do not reintroduce
client-side read state).

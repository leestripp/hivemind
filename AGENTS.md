# AGENTS.md — HiveMind Node Graph

> Quick-start reference for AI coding agents. Read this before touching code.
> Deeper design rationale: `docs/design.md`. Current work queue: `NEXT.md`.

---

## 1. Project Overview

**HiveMind Node Graph** is a visual **multi-agent orchestration system**. Users
compose workflows on a canvas (vanilla-JS SPA in `web/dist/`); each node is one
unit of work; edges carry text between nodes. A C++23 server walks the graph in
topological order and executes each node — the `agent` node type embeds a full
`daedalus::Agent` (LLM streaming, tool calls, MCP, skills).

| Property | Value |
|----------|-------|
| Language | **C++23** (`std::jthread`, `std::stop_token`, `std::expected`) |
| Compiler | GCC 13+ / Clang 16+ |
| Build | **CMake 3.28+** (`start.sh` wraps common flows) |
| HTTP server | **OmniNet** `OmniNet::HTTP::Server` (FetchContent, `main` branch) |
| Agent engine | **daedalus** `libdaedalus.a` (FetchContent, tag `0.9.6`) |
| JSON | nlohmann_json |
| DB | SQLite3 — graphs at `~/.local/share/hivemind/hivemind.db` |
| Model settings | **daedalus SettingsStore** — `~/.local/share/daedalus/daedalus.db` |
| Web UI | Vanilla JS, **no build step**, dark theme (`color-scheme: dark`) |
| License | MIT (© Lee A. Stripp) |

### Core concepts

- **Node** — unit of work. `id`, `type`, `label`, `config` (JSON), `x/y`.
  Ports are **derived from `type`** at runtime (`Node::ports_for_type`), never stored.
- **Edge** — `source_node:source_port → target_node:target_port`. Pure wiring; values
  live in the runner's `resolved_` map.
- **Run** — one execution, UUID `run_id`. Status `running | completed | failed | cancelled`.
- **Executor** — one C++ class per node type, registered in `src/ExecutorRegistry.cpp`.

### Node types (8)

| Type | In | Out | Notes |
|------|----|-----|-------|
| `input` | — | `out` | Reads `config.param_name` from the run payload; falls back to `config.value` |
| `prompt` | — | `out` | Static text `config.value` |
| `agent` | `in` | `out` | Embedded `daedalus::Agent` (one per execution). Config: `model`, `project`, `system_prompt`, `auto_approve`, `mcp_enabled`, `tools_disabled[]`, `skills_disabled[]`, `mcp_servers_disabled[]` |
| `merge` | `in_a`, `in_b`, `in_c` | `out` | Joins inputs (`concat` strategy) |
| `loop_start` | `in` | `out`, `loop_count` | Loop entry; `config.iteration_count` |
| `loop_end` | `in`, `loop_count` | `out` | Jumps back while `current < target`; body text feeds back via runner `loop_feedback_` (no stored cycle) |
| `output` | `in` | — | Collects into run results keyed by `config.output_name` |
| `log` | `in` | — | Writes to server stderr |

**There is intentionally no `llm` node** (removed 2026-08-06). All LLM work goes
through `agent` / daedalus. Do not re-add one. Multi-agent DAGs (fan-out → merge →
fan-out) are the normal pattern; each `agent` node is an isolated `daedalus::Agent`.

---

## 2. Tech Stack & Dependencies

System packages (Ubuntu):
```bash
sudo apt install -y cmake g++ libcurl4-gnutls-dev nlohmann-json3-dev \
    sqlite3 libsqlite3-dev libssl-dev libpng-dev libpoppler-cpp-dev \
    clang-tidy   # optional, for lint
```

CMake `FetchContent` pulls **daedalus** (`gitlab.com/leestripp/daedalus`,
`DAEDALUS_TAG`, default `0.9.6`) and **OmniNet** (`gitlab.com/leestripp/omninet`,
`main`). Both land in `build/_deps/` — never edit that tree. CMake options:
`-DHIVEMIND_USE_DAEDALUS=OFF`, `-DDAEDALUS_TAG=…`, `-DDAEDALUS_LOCAL=/path`,
`-DOMNINET_LOCAL=/path`.

**Never patch OmniNet or daedalus source.** If an API misbehaves, work around it
in our code using their public APIs. (Historical note: `cmake/patch_omninet.cmake`
existed and was deleted; `src/routes/` and `src/web/` also no longer exist.)

---

## 3. Build, Run, Test, Lint

```bash
# Convenience wrapper
./start.sh --build            # configure + build + start (port 8421)
./start.sh --lint             # clang-tidy dry run

# Direct
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j$(nproc)
./build/hivemind --port 8421

# Tests (3 CTest targets, all should pass)
ctest --test-dir build --output-on-failure
./build/tests/test_loop_engine      # LoopEngine iteration/feedback
./build/tests/test_node_ports       # ports_for_type for all 8 types
./build/tests/test_daedalus_smoke   # construct daedalus::Agent, no I/O
```

The server runs at `http://localhost:8421/` (UI) and `/api/*` (REST). CLI:
`--port N` / `-p N`, or `HIVEMIND_PORT`. Runs stream via **HTTP polling**
(`GET /api/runs/:rid/output`) — WebSocket `/ws/runs` is planned, not built.

---

## 4. Project Structure

```
CMakeLists.txt          # root build; FetchContent daedalus + OmniNet; lint targets
start.sh                # build/run/lint wrapper
AGENTS.md  NEXT.md  Plan.md  docs/design.md
cmake/clang.cmake       # strict clang toolchain
include/hivemind/
  Graph.h               # Node, Port, Edge, Graph, NodeResult, GraphRun
  GraphStore.h          # SQLite persistence (per-call connections, WAL + busy_timeout)
  GraphRunner.h         # topo executor; state_mtx_ guards resolved_/completed_/ready_
  LoopEngine.h          # per-loop counter + feedback text
  ExecutorRegistry.h    # type-string → NodeExecutor* (PIMPL)
  NodeExecutor.h        # abstract base (execute() + type_name())
  Settings.h            # CLI args (port)
  executors/<X>Executor.h
  routes/<X>Routes.{h,cpp}   # see below
src/
  main.cpp              # server setup; registers routes IN ORDER (static catch-all LAST)
  Graph.cpp             # ports_for_type — single source of port shapes
  GraphStore.cpp        # schema + CRUD; output rows keyed by output_name (empty port)
  GraphRunner.cpp       # ready-queue, parallel dispatch, loop re-schedule, run_sync is BLOCKING
  LoopEngine.cpp  ExecutorRegistry.cpp  NodeExecutor.cpp  Settings.cpp
  executors/<X>Executor.cpp
tests/                  # 3 ctest targets; CMakeLists links daedalus + OmniNet archives
web/dist/               # index.html, app.js, styles.css — served raw, no build
```

Route groups (`include/hivemind/routes/`): `GraphRoutes` (CRUD + `/api/node-types`),
`RunRoutes` (run + polling output), `WorkflowRoutes` (export/import `.hivemind.json`),
`ModelRoutes` (from daedalus SettingsStore), `AgentRoutes` (tools/status/skills/mcp
introspection — all serialized behind `g_introspection_mtx`), `ProjectRoutes`
(`/api/projects` + `/api/fs/browse` directory listing), `StaticRoutes` (custom file
handler, registered last as `/*`).

---

## 5. Code Style & Conventions

- Types `PascalCase`, functions/vars `snake_case`, globals `g_`, `#pragma once`.
- `std::jthread` + `stop_token` for workers; never raw `std::thread`.
- `nlohmann::json` aliased `using json = ...` per TU. Parse defensively (`std::optional`).
- Executors throw `std::runtime_error`/`std::invalid_argument`; the runner catches and
  fails the run.
- SQLite: fresh connection per `GraphStore` call; never share `sqlite3*` across threads.
- **Multi-param routes: do NOT use `req.pathParam(i)`** — OmniNet 0.9.7 stores params in
  an unordered_map and indexed lookup returns them in hash order (swapped). Use the
  `path_segments()` helper in the route file and index positional segments instead.
  Single-param routes are fine.
- New node type checklist: `Graph.cpp::ports_for_type` → executor `.h/.cpp` → register in
  `ExecutorRegistry.cpp` → `main` + `tests` CMake source lists → `test_node_ports.cpp`
  assertion → `NODE_DEFS` + `FIELD_SCHEMAS` in `web/dist/app.js` → docs.

## 6. Hard-Won Invariants (don't regress)

1. **`GraphRunner::run_sync()` is blocking** — it calls `run()` on the caller's thread.
   Spawning a local `jthread` inside it cancels the run instantly (dtor requests stop).
2. **GraphRunner shared state** (`resolved_`, `completed_`, `ready_`) is guarded by
   `state_mtx_` — parallel node dispatch races without it.
3. **daedalus introspection endpoints are serialized** (`g_introspection_mtx` in
   AgentRoutes) — concurrent `Agent` construction races on shared global state (was SIGSEGV).
4. **Unknown agent models fail fast** — `AgentExecutor` validates `config.model` against
   `SettingsStore::resolve_model()` before constructing; don't remove that check.
5. **UI grid tracks are explicitly placed** (`grid-row`/`grid-column` in styles.css) —
   without it, hiding a splitter (`display:none`) shifts panels into the 0px track and
   collapsed panels vanish. `writing-mode` belongs only on `.strip-label`, never the
   strip container (collapses its width to 0).
6. **`:root { color-scheme: dark }`** in styles.css is what keeps native selects/options/
   scrollbars dark. Don't remove.
7. Drag/pan/zoom handlers update the dragged node div + `renderEdges()` only — full
   `render()` mid-gesture destroys the grabbed element and breaks wires.
8. `Inspector` field edits call `refreshNodeChrome(n)`, not `render()` (focus loss).

## 7. Where to Look When…

| Task | File |
|------|------|
| Change a node's behavior | `src/executors/<X>Executor.cpp` |
| Add/rename ports | `src/Graph.cpp::ports_for_type` + `tests/test_node_ports.cpp` |
| Change run orchestration | `src/GraphRunner.cpp` |
| Change persistence | `src/GraphStore.cpp::ensure_schema` |
| Add REST endpoint | pick a route group in `include/hivemind/routes/`; register in `src/main.cpp` BEFORE `register_static_routes` |
| Model/provider config | daedalus SettingsStore (`/api/models`); manage via icarus CLI or `SettingsStore` API |
| UI node palette/inspector | `web/dist/app.js` (`NODE_DEFS`, `FIELD_SCHEMAS`, agent inspector) |
| Bump daedalus | `cmake -DDAEDALUS_TAG=<tag> -S . -B build` |

## 8. Out of Bounds

- `.daedalus/` — agent runtime private state. Never touch.
- `build/` (incl. `build/_deps/`) — generated; don't edit, don't commit.
- daedalus / OmniNet source — see §2. Work around, never patch.
- `hive-agent/`, `prototype/`, `src/routes/`, `src/web/` — deleted; recover from git history only.

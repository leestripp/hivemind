# AGENTS.md — HiveMind Node Graph

> Quick-start reference for AI coding agents (CodeMax, Claude-Code, etc.)
> working on this codebase. For the full design rationale, execution model,
> multi-agent patterns, and worked examples see `docs/design.md`. For executable
> build / lint / start commands see `start.sh`.

---

## 1. Project Overview

**HiveMind Node Graph** is a visual workflow editor built around directed
graphs of small, single-purpose **nodes**. Users compose workflows on a canvas
(a vanilla JS SPA served from `web/dist/`); each node performs one action (LLM
call, agent run, loop iteration, input/output, log); edges carry text between
them. The C++23 server executes the graph in-process, dispatching each node to
a type-specific executor. The only node that does real reasoning is `agent`,
which delegates to an embedded [`daedalus`](https://gitlab.com/leestripp/daedalus)
AI-agent library.

| Property | Value |
|----------|-------|
| Language | **C++23** (`std::jthread`, `std::stop_token`, `std::ranges`, `std::expected` where supported, etc.) |
| Compiler | GCC 13+ / Clang 16+ |
| Build system | **CMake 3.28+** |
| HTTP server | **OmniNet** (gitlab.com/leestripp/omninet) via `FetchContent` |
| JSON | nlohmann_json |
| Database | SQLite3 (file `~/.local/share/hivemind/hivemind.db`) |
| HTTP client | libcurl (SSE streaming for LLM) |
| Embedded AI agent | `libdaedalus.a` (target `daedalus`) via `FetchContent` |
| Web UI | Vanilla JS SPA in `web/dist/` (no framework / build step) |
| License | MIT (Copyright © 2024 Lee A. Stripp) |

### Core concepts

- **Node** — a unit of work. Has typed input/output ports and a `type` string.
- **Port** — `string` (only type supported today). Derived per type, not stored.
- **Edge** — `source_node + source_port → target_node + target_port`. Carries a string value at runtime.
- **Graph** — collection of nodes + edges, persisted in SQLite.
- **Run** — a single execution of a graph. `run_id` (UUID). Status: `running | completed | failed | cancelled`.
- **Runner** — the orchestrator. Walks the graph in topological order and dispatches each node's executor when its inputs are ready. The runner is **not** an AI agent; agents are just one node type among many.

### Node types (v1)

| Type | Inputs | Outputs | Purpose |
|------|--------|---------|---------|
| `prompt`    | — | `out`   | Static text value (`config.value`) |
| `agent`     | `in`    | `out`   | AI-agent run via embedded `daedalus::Agent` (tool calls, MCP, skills, streaming) |
| `loop_start`| `in`    | `out`, `loop_count` | Loop entry: passes `in` to `out`; emits `loop_count` to paired `loop_end`. Receives body text feedback from `loop_end` on iteration > 1. |
| `loop_end`  | `in`, `loop_count` | `out` | Loop exit: receives body text and count. If `current < target`, runner feeds body text back to `loop_start`; else emits `out`. |
| `input`     | — | `out`   | Reads a named key from the `POST /api/graphs/:gid/run` body |
| `output`    | `in`    | —       | Collects a result for the run payload |
| `log`       | `in`    | —       | Prints to server stderr |

Unknown types return empty port lists. Port derivation lives in
`src/Graph.cpp` (`Node::ports_for_type`).

> **Important architectural note:** the **runner** walks the graph and
> dispatches nodes. `agent` nodes are **passive peers** — they fire on
> input-ready the same way `prompt`/`log` do. The AI agent does **not**
> orchestrate the graph. Multiple `agent` nodes in one graph run as parallel
> independent `daedalus::Agent` instances. See `docs/design.md` §3.3 / §4.2 /
> §6.

---

## 2. Tech Stack

| Layer | Library / Tool | How it's wired |
|-------|----------------|----------------|
| HTTP server | **OmniNet** (`OmniNet::HTTP::Server`) | CMake `FetchContent` (or `OMNINET_LOCAL` override). Provides the REST API and serves the static web UI. |
| JSON | **nlohmann_json** | `find_package(nlohmann_json REQUIRED)`; also via `FetchContent` as fallback. |
| SQLite | **sqlite3** | `pkg_check_modules(SQLite3 REQUIRED sqlite3)`. |
| HTTP client | **CURL::libcurl** | `find_package(CURL REQUIRED)`. |
| Threads | `std::jthread`, `std::stop_token`, `std::async` | Header-only, no external dep. |
| AI agent engine | **`libdaedalus.a`** (target `daedalus`) | Fetched via CMake `FetchContent` from `gitlab.com/leestripp/daedalus` (default tag `0.9.6`). Exposes `daedalus::Agent` and the `daedalus::AgentSink` callback interface. Override with `-DDAEDALUS_TAG=...` or `-DDAEDALUS_LOCAL=...`. |
| Web UI | **Vanilla JS** (`index.html`, `app.js`, `styles.css`) | Served from disk by a custom catch-all route; files are read with `std::filesystem` and returned via `OmniNet::HTTP::Response`. |
| Lint | **clang-tidy** | Configured by `.clang-tidy`. Build emits `compile_commands.json` and a `lint` target. |

### System packages (Ubuntu/Debian)

```bash
sudo apt install -y cmake g++ libcurl4-gnutls-dev \
                    nlohmann-json3-dev sqlite3 libsqlite3-dev \
                    libssl-dev libpng-dev libpoppler-cpp-dev \
                    clang-tidy            # optional, for --lint
```

- `libssl-dev`, `libpng-dev`, `libpoppler-cpp-dev` are required by **daedalus**.
- `clang-tidy` is only needed if you run `./start.sh --lint`.

### CMake options

| Option | Default | Meaning |
|--------|---------|---------|
| `HIVEMIND_USE_DAEDALUS` | `ON` | Fetch and link daedalus as the AI agent engine. |
| `DAEDALUS_TAG` | `0.9.6` | Git tag / branch / SHA for daedalus. |
| `DAEDALUS_LOCAL` | — | Path to a local daedalus checkout (skip the fetch). |
| `OMNINET_LOCAL` | — | Path to a local OmniNet checkout (skip the fetch). |

---

## 3. Build, Run, Test, Lint

The project ships a `start.sh` convenience script that wraps the common
workflows. CMake works directly too.

### Convenience (`start.sh`)

```bash
./start.sh                              # run with existing build (default port 8421)
./start.sh --build                      # configure + build + start
./start.sh --clang --build              # build with clang/clang++
./start.sh --clang --build --clean      # wipe build/ first
./start.sh --build --no-start           # build only
./start.sh --port 9000                  # override port (also via HIVEMIND_PORT env)
./start.sh --lint                       # dry-run clang-tidy (needs compile_commands.json)
./start.sh --lint-fix                   # apply clang-tidy fixes
./start.sh --help
```

`start.sh` always configures with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` so
clang-tidy works out of the box. Build type defaults to `Release` (-O3).

### Direct CMake

```bash
# Configure
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Or use the clang toolchain file
cmake -S . -B build-clang \
      -DCMAKE_TOOLCHAIN_FILE=cmake/clang.cmake \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Build
cmake --build build -j$(nproc)

# Run
./build/hivemind --port 8421
```

The binary `build/hivemind` listens on `0.0.0.0:8421` by default. CLI flags:
`--port N | -p N`, `--help | -h`.

### Tests (CTest)

```bash
# Build everything (tests are part of the default build)
cmake --build build -j$(nproc)

# Run all tests
ctest --test-dir build --output-on-failure

# Run a single test directly
./build/tests/test_loop_engine
./build/tests/test_node_ports
```

Existing tests live in `tests/`:
- `test_loop_engine.cpp` — LoopEngine iteration / reset
- `test_node_ports.cpp` — `Node::ports_for_type` for every node type

The macro that builds each test target (`hivemind_test`) is in
`tests/CMakeLists.txt`. It links the project's own `.cpp` files plus the
`daedalus` and `OmniNet` static archives (from `build/_deps/`).

### Lint (clang-tidy)

```bash
# After a configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON:
cmake --build build --target lint       # dry run, writes lint-results.yaml
cmake --build build --target lint-fix   # auto-fix; review with git diff
```

`clang-tidy` config is in `.clang-tidy`. Headers matching `src/.*` or
`include/hivemind/.*` are checked; `_deps/*` and `build*/*` are
excluded. Checks enabled: `clang-diagnostic-*`, `clang-analyzer-*`,
`performance-*`, `readability-*`, `modernize-*`, `cppcoreguidelines-*` (with
selective exclusions for magic numbers, init-variables, regex, etc.).

### Install

`cmake --install build` writes to `~/.local/bin/hivemind` (the default
`CMAKE_INSTALL_PREFIX` set in `CMakeLists.txt`).

---

## 4. Project Structure

```
Hive-Mind/
├── CMakeLists.txt                    # Root build script
├── start.sh                          # Convenience build / start / lint
├── Plan.md                           # (optional) migration plan; not currently in repo.
├── docs/
│   └── design.md                     # Full design doc: execution model, multi-agent DAGs, worked examples
├── LICENSE                           # MIT
├── .clang-tidy                       # clang-tidy config
├── .gitignore                        # ignores build/, .codemax/, prompt.txt
│
├── cmake/
│   └── clang.cmake                   # Clang toolchain (extra warnings: -Wconversion -Wshadow …)
│
├── include/hivemind/                 # Public headers (consumed by src/ + tests/)
│   ├── Graph.h                       # Node, Port, Edge, GraphMeta, Graph, NodeResult, GraphRun
│   ├── GraphStore.h                  # SQLite persistence layer
│   ├── NodeExecutor.h                # Abstract base for node executors
│   ├── ExecutorRegistry.h            # Type-string → NodeExecutor* (PIMPL)
│   ├── LoopEngine.h                  # Per-loop iteration counters
│   ├── AgentSpawner.h                # (legacy) subprocess wrapper — unused
│   ├── GraphRunner.h                 # Topological exec engine (the conductor)
│   ├── LLMSettings.h                 # Reads ~/.codemax/settings/settings.json
│   ├── Settings.h                    # Server settings (port only)
│   ├── executors/                    # One .h per node type
│   │   ├── PromptExecutor.h
│   │   ├── AgentExecutor.h           # AI Agent run via daedalus::Agent
│   │   ├── LoopStartExecutor.h
│   │   ├── LoopEndExecutor.h
│   │   ├── InputExecutor.h
│   │   ├── OutputExecutor.h
│   │   └── LogExecutor.h
│   └── routes/                       # One .h + .cpp per route group
│       ├── GraphRoutes.{h,cpp}       # /api/graphs CRUD
│       ├── RunRoutes.{h,cpp}         # /api/graphs/:gid/run + /api/runs/:rid
│       ├── StaticRoutes.{h,cpp}      # GET /, /app.js, /styles.css
│       ├── ModelRoutes.{h,cpp}       # /api/models
│       ├── ProjectRoutes.{h,cpp}     # /api/projects (scan codemax + local)
│       └── AgentRoutes.{h,cpp}       # /api/agent/tools, /api/agent/status
│
├── src/                              # Implementation for the hivemind executable
│   ├── main.cpp                      # Entry point: OmniNet HTTP server, signal handling
│   ├── Graph.cpp                     # Node::ports_for_type()
│   ├── GraphStore.cpp                # SQLite CRUD + schema
│   ├── GraphRunner.cpp               # topo_sort + ready-queue + parallel exec
│   ├── LoopEngine.cpp
│   ├── ExecutorRegistry.cpp          # Build the type → executor map
│   ├── NodeExecutor.cpp              # Stub (base is pure-virtual)
│   ├── AgentSpawner.cpp              # Legacy subprocess wrapper (kept for back-compat)
│   ├── LLMSettings.cpp               # Parse ~/.codemax/settings/settings.json
│   ├── Settings.cpp                  # CLI arg parsing
│   └── executors/                    # .cpp for each executor
│       ├── PromptExecutor.cpp
│       ├── AgentExecutor.cpp         # Calls daedalus::Agent (sinks events to output stream)
│       ├── LoopStartExecutor.cpp
│       ├── LoopEndExecutor.cpp
│       ├── InputExecutor.cpp
│       ├── OutputExecutor.cpp
│       └── LogExecutor.cpp
│
├── tests/                            # CTest targets for hivemind
│   ├── CMakeLists.txt                # hivemind_test() macro
│   ├── test_loop_engine.cpp
│   └── test_node_ports.cpp
│
│   ├── graph_node_system.cpp         # Minimal node-graph runtime demo
│   └── README.md
│
├── web/dist/                         # Vanilla JS SPA (no build step)
│   ├── index.html
│   ├── app.js
│   └── styles.css
│
├── resources/                        # Icons / banner
│   ├── banner.png
│   ├── icon.png
│   └── icon_192x192.jpg
│
├── build/                            # CMake build dir (gitignored)
│   └── _deps/
│       ├── daedalus-src/             # FetchContent-cloned daedalus source
│       ├── daedalus-build/           # daedalus build artifacts
│       ├── omninet-src/              # FetchContent-cloned OmniNet source
│       └── omninet-build/            # OmniNet build artifacts
│
└── lint-results.yaml                 # Last clang-tidy output (created by --lint)
```

---

## 5. Code Style and Conventions

### Language / idioms

- **C++23, aggressively.** Use `std::jthread` + `std::stop_token` for any
  background worker; never raw `std::thread`. Prefer `std::async` for one-shot
  parallel work. Use `std::filesystem`, `std::chrono`, `std::ranges`.
- **No busy-waiting.** Cancellation is cooperative via `std::stop_token`.
- **Exceptions** are thrown for fatal executor errors and caught at the
  `GraphRunner` boundary (run is marked `failed`).
- **`std::optional`** for fallible getters (e.g. `GraphStore::get_graph`).
- **`nlohmann::json`** aliased to `json` in every TU that uses it.

### Headers and layout

- Public API lives in `include/hivemind/` (note the `hivemind/` namespace dir).
- Sources live in `src/`. Route handlers split into
  `include/hivemind/routes/<X>Routes.{h,cpp}`.
- One `NodeExecutor` subclass per node type; each declared in
  `include/hivemind/executors/<X>Executor.h` and implemented in
  `src/executors/<X>Executor.cpp`. Registered in
  `src/ExecutorRegistry.cpp` (single source of truth for type → executor).
- Headers use `#pragma once`. Forward declarations over includes where possible.
- Every header that has non-obvious thread-safety rules says so in a Doxygen
  comment block at the top (follow the existing pattern).

### File header template

```cpp
#pragma once

#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

/**
 * One-line summary.
 *
 * Longer description of responsibilities and thread-safety contract.
 * Used by: caller files
 */

using json = nlohmann::json;
```

### Naming

- Types: `PascalCase` (`GraphStore`, `NodeResult`, `ModelConfig`).
- Functions / variables: `snake_case` (`create_graph`, `run_id`).
- Free functions for executor registration: `register_<thing>_routes(...)`. The
  HTTP server is passed as `OmniNet::HTTP::Server&`.
- Globals: prefixed `g_` (`g_shutdown`, `g_runs`).
- Constants / macros: `UPPER_SNAKE_CASE` only for `#define`s; prefer `constexpr`.

### Route handlers

Pattern used throughout `include/hivemind/routes/*.cpp`:

```cpp
server.get("/api/foo/:id",
    [](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
        std::string id{req.pathParam("id")};
        // … do work
        resp = OmniNet::HTTP::Response::ok();
        resp.setContentType("application/json");
        resp.setBody(json{{"id", id}}.dump());
    });
```

Use `OmniNet::HTTP::Response::ok() / created() / badRequest() / notFound() / internalError(...)`
helpers; always set `Content-Type` and a JSON body. Catch-all static routes must be
registered **last** so specific API routes match first (see `StaticRoutes.cpp`).

### Error handling

- Parse JSON defensively: `parse_body(...)` returns `std::optional<json>`.
- Executors throw `std::runtime_error` / `std::invalid_argument`; the runner
  catches them and emits failure events.
- SQLite errors propagate as exceptions out of `GraphStore` methods.

### Logging

- `std::cout` for normal startup messages.
- `std::cerr` for warnings and caught exceptions.
- No logging framework — keep it simple.

### Clang-tidy

`.clang-tidy` is checked into the repo. CI-friendly command:

```bash
cmake --build build --target lint     # dry run; non-zero exit only if errors
```

`WarningsAsErrors` is empty by default — clang-tidy reports are advisory
unless you change it. Do **not** exclude the `_deps` or `build*` directories
from scratch; the existing config already does so.

---

## 6. Key Subsystem Notes (for new contributors)

### Adding a new node type

1. Add a `case` in `src/Graph.cpp::Node::ports_for_type`.
2. Add an executor header in `include/hivemind/executors/<X>Executor.h`
   subclassing `NodeExecutor`.
3. Implement it in `src/executors/<X>Executor.cpp`.
4. Register it in `src/ExecutorRegistry.cpp`.
5. Add a test in `tests/` using the `hivemind_test()` macro.
6. Update the **Node types** table in `docs/design.md` §3.1.
7. Document `config_json` shape in `docs/design.md` §3.2.

### Adding a new API route

1. Decide which group it belongs to (`Graph`, `Run`, `Model`, `Project`, `Agent`, or new group).
2. If new group: add `include/hivemind/routes/<X>Routes.{h,cpp}`, declare
   `register_<x>_routes(OmniNet::HTTP::Server&, ...)`, and call it from `src/main.cpp`.
3. If extending a group: edit the existing `<X>Routes.cpp`.
4. Update `docs/design.md` §5.1 REST surface table.

### SQLite schema changes

- `GraphStore::ensure_schema()` creates tables idempotently. For schema
  evolution, add a migration step there (no migration framework is in place).
- DB file lives at `~/.local/share/hivemind/hivemind.db` (created on first
  access). Delete it to start fresh during development.
- Connections are **not** thread-safe; each `GraphStore` method opens its own
  short-lived connection. For parallel execution within a run, use the per-run
  state in `GraphRunner::resolved_` and flush to SQLite at end of run.

### LLM model configuration

Models are configured externally in `~/.codemax/settings/settings.json`
(see the codemax / icarus tool ecosystem). HiveMind reads them via `LLMSettings`
(singleton: `llm_settings()`). Falls back to env vars `HIVEMIND_BASE_URL`,
`HIVEMIND_API_KEY`, `HIVEMIND_MODEL` if the file is absent. The model `name`
field is what the UI shows and what the `agent` nodes store in their
`config.model`.

### daedalus embedding (the `agent` node)

The `agent` node constructs a fresh `daedalus::Agent` per execution. The
executor implements `daedalus::AgentSink` to capture:

- `on_assistant_delta(text)` — streamed assistant tokens (appended to the
  node's output buffer)
- `on_tool_call(name, args)` / `on_tool_result(name, result)` — tool-call
  loop (summarised into the output buffer)
- `on_state_change(from, to)` / `on_compaction(before, after)` — lifecycle events
- `on_error(kind, msg, will_retry)` — errors
- `on_log(level, msg)` — forwarded to `std::cerr`
- `request_command_approval(cmd)` — returns `true` (auto-approve in the node-graph context)

This is intentionally simple: one `Agent` per `agent`-node execution, no shared
state between nodes, deterministic teardown via RAII. The full design contract
is in `docs/design.md` §6.

### OmniNet HTTP server

The server is `OmniNet::HTTP::Server`. Routes are registered directly on it
(`server.get(...)`, `server.post(...)`, etc.). The static web UI is served by a
custom file handler registered **last** as a catch-all `/*`. The handler maps
request paths to files under `web/dist/`, rejects traversal attempts, and sets
content types by extension. If you need WebSocket support in the future, use
`OmniNet::HTTP::Server::registerProtocolSwitch` (see OmniNet docs); do **not**
patch OmniNet.

---

## 7. Anti-patterns to Avoid

1. **Don't re-implement LLM streaming or the agent loop** — there is no
   "plain LLM call" node type by design. The `agent` node embeds
   `daedalus::Agent` (tool calls, MCP, skills, streaming); daedalus owns
   all LLM transport. No subprocess for LLM calls.

2. **Don't store model credentials** in HiveMind. Read from
   `~/.codemax/settings/settings.json` or env vars.
3. **Don't block the HTTP request thread** during graph execution. The
   `/api/graphs/:gid/run` handler returns `202` immediately; the runner runs
   on a detached thread.
4. **Don't use raw `std::thread`** — use `std::jthread` or `std::async`.
5. **Don't share `sqlite3*` across threads.** Open a fresh connection per
   short operation, or use a mutex.
6. **Don't edit files inside `build/_deps/daedalus-src/`** — that directory is
   `FetchContent`'d at configure time and is wiped on every clean build. Modify
   daedalus upstream and bump `DAEDALUS_TAG` instead.
7. **Don't share `daedalus::Agent` instances across threads or across
   agent-node executions.** The contract is *one Agent per `agent` node
   execution*. Construct it, drive it, let it destruct. Sharing breaks the
   cancellation and lifecycle invariants in `docs/design.md` §6.
8. **Don't patch OmniNet.** If the public API doesn't expose what you need,
   find an alternative approach using the existing API or ask. Patching will be
   overwritten on the next `FetchContent` fetch.
9. **Don't commit `build/`, `lint-results.yaml`** — they are gitignored by
   intent. Build artefacts (including `build/_deps/daedalus-src/` and
   `build/_deps/omninet-src/`) belong outside the tree.

---

## 8. Quick Recipes

### Run the server against a fresh DB

```bash
rm -f ~/.local/share/hivemind/hivemind.db
./start.sh --build --clean
```

### Check if a graph will run (cycle detection)

`GraphRunner` rejects graphs containing cycles. Build a graph in the UI and hit
**Run**; the error appears in the run output.

### Poll a run's live output

```bash
# Start a run
curl -s -X POST http://localhost:8421/api/graphs/<gid>/run \
     -H 'Content-Type: application/json' \
     -d '{"inputs":{"topic":"Hello"}}'

# Poll the output buffer
watch -n 0.5 'curl -s http://localhost:8421/api/runs/<rid>/output | jq .'

# Get final status + results
curl -s http://localhost:8421/api/graphs/<gid>/runs/<rid> | jq .
```

### Add a new dependency

For a system package: add `find_package(...)` or `pkg_check_modules(...)`
to `CMakeLists.txt` and link via `target_link_libraries(hivemind PRIVATE ...)`.

For another header-only / FetchContent library: declare a `FetchContent_Declare`
block in `CMakeLists.txt`, call `FetchContent_MakeAvailable(...)`, and link
the resulting target.

---

## 9. Where to Look When …

| You want to … | Look at |
|---------------|---------|
| Understand the full design rationale | `docs/design.md` |
| Understand the migration plan | `Plan.md` |
| Change a node's behaviour | `src/executors/<X>Executor.cpp` + `include/hivemind/executors/<X>Executor.h` |
| Add a node type | `src/Graph.cpp` (ports) + new executor + `src/ExecutorRegistry.cpp` |
| Change the persistence schema | `src/GraphStore.cpp::ensure_schema()` |
| Change how graphs execute | `src/GraphRunner.cpp` (topo sort + ready queue) |
| Change loop semantics | `src/LoopEngine.cpp` + `src/executors/{LoopStart,LoopEnd}Executor.cpp` |
| Add a REST endpoint | `include/hivemind/routes/<X>Routes.cpp` and call its `register_*` from `src/main.cpp` |
| Add an LLM provider | `include/hivemind/LLMSettings.h` / `src/LLMSettings.cpp` (read `~/.codemax/settings/settings.json`) |
| Tweak UI serving | `include/hivemind/routes/StaticRoutes.cpp` (looks for `web/dist/`) |
| Diagnose OmniNet API questions | `build/_deps/omninet-src/docs/api/` after the first configure |
| Run lint | `./start.sh --lint` (requires `compile_commands.json` in `build/`) |
| Bump the agent engine | `cmake -S . -B build -DDAEDALUS_TAG=<branch-or-sha>` |

---

## 10. Out-of-Bounds Directories

These directories are **off-limits** unless the user explicitly asks:

- `.daedalus/` — agent runtime private state. Never delete, rename, edit, or
  include in cleanup tasks. See the `.daedalus*` rules in the host's system prompt.
- `build/_deps/daedalus-src/` / `build/_deps/omninet-src/` — CMake FetchContent
  clones. Wiped on every clean build. Don't edit; don't commit.
- `build/` — CMake build directory; safe to `rm -rf` only when the user
  requests a clean build.
- `hive-agent/` — no longer exists; it was deleted after the migration to daedalus.
  If you still need the old code, recover it from git history.

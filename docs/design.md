# HiveMind 2.0 — Design

> **Audience:** contributors, reviewers, and AI coding agents picking up the project.
> **Scope:** what HiveMind 2.0 is, how the pieces fit, and the decisions that
> constrain changes. Roadmap and phase status live in `../Plan.md`.

---

## Contents

1. [Overview](#1-overview)
2. [Repository layout](#2-repository-layout)
3. [Server architecture](#3-server-architecture)
4. [Roster agents](#4-roster-agents)
5. [Threads & chat flow](#5-threads--chat-flow)
6. [Containers (agent sandboxes)](#6-containers-agent-sandboxes)
7. [Collaboration model](#7-collaboration-model)
8. [Approvals](#8-approvals)
9. [Routines & skills](#9-routines--skills)
10. [GTK client](#10-gtk-client)
11. [Graph workflow engine (1.x, kept)](#11-graph-workflow-engine-1x-kept)
12. [Persistence map](#12-persistence-map)
13. [Concurrency rules](#13-concurrency-rules)
14. [Extension points](#14-extension-points)

---

## 1. Overview

HiveMind 2.0 is a **self-hosted team of always-on AI agents for Linux**.
The unit of the product is the **roster agent**: a persistent, named
teammate you message like a colleague. Agents remember their conversations,
run commands in their own podman containers, coordinate with each other,
and pause for human approval when configured to.

Two first-party libraries do the heavy lifting (both
gitlab.com/leestripp, consumed as-is — **never patched**):

- **daedalus** — the agent engine (`daedalus::Agent`): LLM streaming, tools,
  MCP, skills, memory, context compression. Owns all model/provider/credential
  state in its SettingsStore.
- **OmniNet** — the HTTP/WebSocket server.

A third project, **icarus**, is our own CLI agent built on daedalus; it is
the reference implementation for how a daedalus host should be wired, and
can be spawned per-role via the `run_icarus` tool.

**Platform:** Linux only (Ubuntu 26.04 LTS target). No Windows/macOS.

## 2. Repository layout

```
Hive-Mind/
├── CMakeLists.txt            # superbuild: deps (daedalus, OmniNet) + subdirs
├── hivemindctl               # build/start/stop/restart/status/logs/client/test
├── hive-server/
│   ├── include/hivemind/     # headers (+ routes/*.cpp live beside headers)
│   ├── src/                  # server sources
│   └── data/                 # Containerfiles, systemd unit
├── hive-client/              # GTK4/gtkmm4 native client
├── web/dist/                 # web UI (workflow studio + zero-install fallback)
├── tests/                    # ctest suite (linked against hivemind_core)
├── workflows/                # sample .hivemind.json graphs
└── docs/
```

`hive-server` builds two targets: `hivemind_core` (static lib, everything
except `main.cpp` — also what tests link) and the `hive-server` binary.

## 3. Server architecture

One process, one port (default 8421), all state on the local machine:

```
OmniNet HTTP server
 ├── Roster/chat routes  → AgentStore / ThreadStore / ChatService
 ├── Approval routes     → ApprovalStore
 ├── Routine routes      → RoutineStore  (fired by the 30 s scheduler tick)
 ├── Container routes    → ContainerManager (podman CLI)
 ├── Graph/run/trigger routes (1.x engine) → GraphStore / GraphRunner
 └── static catch-all    → web/dist (must be registered LAST)
```

Application state is constructed in `main.cpp` and passed to route
registrars by reference: `AgentStore`, `ThreadStore`, `ApprovalStore`,
`RoutineStore`, `AgentRegistry`, `ChatService`, plus the 1.x `GraphStore`
and `ExecutorRegistry`.

**Shutdown path matters:** daedalus only persists conversation history from
`Agent::shutdown()` (its destructor does not save). The SIGTERM/SIGINT path
calls `AgentRegistry::shutdown_all()` before stopping the server.

## 4. Roster agents

A roster row (`agents.db`) is the registry entry: id, unique name, role,
model, workspace_mode, require_approval.

The live runtime is an **AgentHost** (kept in `AgentRegistry`, keyed by id):

- Lazy: the `daedalus::Agent` is constructed on first turn.
- Long-lived: kept alive across turns (construct/destruct churn in daedalus
  0.9.6 is unsafe; see §13).
- Persistent: the host dir `~/.local/share/hivemind/agents/<id>/` is the
  daedalus `project_root`, and the roster name (sanitized) is
  `AgentConfig::agent_name`, so daedalus namespaces messages/memory under
  `<dir>/.daedalus/daedalus-<name>/`. Restart the server and the agent
  resumes with full context.
- The `role` becomes a system-prompt persona preamble.
- **ProxySink:** daedalus takes its sink at construction, so the Agent holds
  a stable ProxySink that forwards to a per-turn StreamingSink (streams
  assistant deltas to the activity endpoint, captures tool calls/errors,
  routes command-approval requests).

## 5. Threads & chat flow

`threads.db` holds threads, agent participants, and messages (microsecond
timestamps keep insertion order; incremental fetch via `?after=<msg_id>`).

Posting a user message:

1. `POST /api/threads/:id/messages` → message stored, **202** returned.
2. `ChatService` spawns a detached thread per agent participant with a
   `LiveTurn` tracker.
3. Each turn: `AgentHost::run_turn(prompt, on_delta)` — deltas append to the
   tracker's `partial` (polled via `/api/threads/:id/activity`).
4. Completion → full reply stored as an agent message; tracker marked done.

Clients poll `activity` (in-progress stream) + `messages?after=` (completed)
— hive-client polls both every 1.5 s. WS push is a deliberate deferral.

## 6. Containers (agent sandboxes)

**Agents never execute on the host.** Workspace modes: `shared` (default —
containerized) or `host` (explicit escape hatch for dev machines without
podman).

- `ContainerManager` wraps the **podman CLI** (no libpod). Containers are
  `hive-<agent_id>`, image `localhost/hive-agent:latest`, auto-built from
  `hive-server/data/Containerfile.hive-agent` on first use.
- **Company workspace model:** `/workspace` is the configured host folder
  (`workspace_root` in server.json, default
  `~/.local/share/hivemind/workspace`; set via Settings page or
  `PUT /api/config`, admin-only) bind-mounted into every agent container;
  `/home/hive` is a per-agent bind mount from the host agent dir.
  Department volumes are a future extension.
- The in-container user is `hive` with **passwordless sudo** — agents
  install their own CLI tools mid-task.
- **ExecBridge:** the built-in host `execute_bash` is disabled for
  containerized agents and replaced by the host-registered tool
  **`container_exec`** (`podman exec -u hive -w /workspace … bash -lc`).
  Distinct name because daedalus' `tools_disabled` also hides host tools
  with the same name.
- If the runtime is unavailable the turn still runs: `container_exec`
  returns a structured error the agent can report.
- Containers run on the server machine. Remote machines are a future
  `hive-worker` mode; exec routing hides behind `ContainerManager`.

A browser/computer-use variant image
(`Containerfile.hive-agent-browser`: chromium/playwright/Xvfb/noVNC) is
shipped for Phase 6 (login handoff, no-API websites).

## 7. Collaboration model

Every roster agent gets these host-registered tools:

| Tool | Purpose |
|---|---|
| `list_agents` | Roster discovery (id, name, role) |
| `message_agent` | Send a task/question to a teammate; runs their turn inline and returns the reply |
| `create_agent` | Spin up a new teammate |
| `run_icarus` | Delegate a self-contained task to an icarus subprocess |
| `container_exec` | (containerized agents) run commands in their sandbox |

`message_agent` is **synchronous but nested**: the target's turn runs inside
the caller's tool call on the same thread. Two guards make this safe:

- `agent_exec_mutex` is a `std::recursive_mutex` (same-thread nesting OK,
  cross-thread still serialized — daedalus 0.9.6 shares global state).
- A thread-local depth counter caps delegation chains at 4.

This enables the **chief-of-staff pattern** without any special code: give
an agent a coordinator role and it will discover, delegate, and even create
specialists via tool calls.

## 8. Approvals

`require_approval=true` on a roster agent flips its daedalus
`auto_approve` off. Then:

- Host `execute_bash`: daedalus asks the sink → `request_command_approval`
  parks a row in `approvals.db` and **blocks the turn** until a human
  resolves it via REST (10 min timeout → `expired`/deny).
- `container_exec`: daedalus doesn't gate host tools, so the tool enforces
  the same queue itself.

Statuses: `pending → approved | denied | expired`.

## 9. Routines & skills

- **Routines** (`routines.db`): named prompt + schedule (once/interval/
  daily/weekly — the 1.x trigger spec). Each routine owns a thread; the 30 s
  scheduler tick posts due prompts into it via `ChatService`, so the agent's
  reply lands in a thread the user can read. A routine that throws is
  disabled rather than re-firing every tick.
- **Skills:** `POST /api/agents/:id/skills` writes an Anthropic-format
  `SKILL.md` into `<agent_dir>/.agents/skills/<name>/` — daedalus discovers
  it automatically. Teach-by-demonstration v1 = narrated steps saved as a
  skill.

## 10. GTK client

`hive-client` (gtkmm-4.0, Ubuntu 26.04): chat-first daily driver.

- `ApiClient`: blocking libcurl+JSON calls, always on worker threads;
  results marshalled to the GTK loop via a `Glib::Dispatcher` queue.
- `HiveWindow`: roster sidebar (double-click → DM), thread list, chat
  transcript (CSS bubbles), composer, approvals inbox with pending badge,
  new-agent/new-thread dialogs, **Workflows** button that opens the web UI
  canvas in the browser (the graph canvas deliberately stays web-only).
- Polling (1.5 s) drives messages/activity/approvals — no WebSocket yet.

## 11. Graph workflow engine (1.x, kept)

The node-graph engine is untouched: `GraphStore` (graphs/nodes/edges),
`GraphRunner` + `LoopEngine` + executors, triggers/webhooks/scheduler,
portable `.hivemind.json` import/export, web canvas in `web/dist`.
Graph `agent` nodes create one-shot daedalus agents per execution — a
different (deliberate) lifecycle from roster agents. Merging the two
(graph nodes referencing roster agents) is a future item.

## 12. Persistence map

| What | Where |
|---|---|
| Graphs/runs/triggers (1.x) | `~/.local/share/hivemind/hivemind.db` |
| Roster | `~/.local/share/hivemind/agents.db` |
| Threads/messages | `~/.local/share/hivemind/threads.db` |
| Approvals | `~/.local/share/hivemind/approvals.db` |
| Routines | `~/.local/share/hivemind/routines.db` |
| Agent runtime state (history, memory, skills) | `~/.local/share/hivemind/agents/<id>/` |
| API keys | `~/.local/share/hivemind/auth.db` (hashes only) |
| Server config (workspace_root) | `~/.local/share/hivemind/server.json` |
| Models/providers/credentials | daedalus SettingsStore (`~/.local/share/daedalus/daedalus.db`) |
| Containers/images/volumes | podman (rootless, per-user) |

## 13. Concurrency rules

1. **daedalus 0.9.6 shares global state** across Agent instances. All
   construct/run/destruct is serialized by the recursive
   `agent_exec_mutex()` (`AgentSerialization.h`). Correct, not concurrent.
   Parallel agent execution needs a daedalus upgrade, not local hacks.
2. One turn at a time per roster agent (`AgentHost::turn_mtx_`).
3. Nested same-thread turns (delegation) are allowed; depth-capped at 4.
4. SQLite stores open a fresh connection per operation — safe from any
   thread, no shared handles.
5. Chat turns run on detached threads; the HTTP handlers return immediately.
6. GTK client: no blocking calls on the main loop, ever.

## 14. Extension points

- **New agent tool:** subclass `daedalus::BaseTool`, register in
  `AgentHost::ensure_agent()`. Use a unique name (never a disabled
  built-in's name).
- **New store:** copy the `AgentStore` pattern (own .db file, fresh
  connection per op, ctor takes a path for tests).
- **New routes:** `routes/FooRoutes.{h,cpp}`, register in `main.cpp`
  **before** `register_static_routes`. Parse multi-param paths with a
  local `path_segments()` helper — OmniNet `pathParam(i)` is unreliable
  for multi-param routes.
- **New test:** `tests/test_foo.cpp` + `hivemind_test(test_foo)` in
  `tests/CMakeLists.txt` (asserts stay live — tests build with `-UNDEBUG`).

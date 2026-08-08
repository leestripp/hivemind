# HiveMind — Next Steps / Working Notes

> Status: 2026-08-08 — all 8 node types shipped and live-verified (agents
> included, against MiniMax-M2.7). UI is feature-rich: agent inspector,
> workflow files, directory browser, panel system. Next milestone: webhook trigger nodes
> (first slice of the trigger roadmap).

## Quick project state

- **Build:** `build/hivemind` compiles cleanly.
- **Tests:** `ctest --test-dir build` → 4/4 pass (LoopEngine, node ports, daedalus smoke, graph runner e2e).
- **Server:** running in dev on `:8421`; start with `./build/hivemind --port 8421`.
- **Git:** clean working tree on `main`.

## Node-type status

| Node type | Backend | Status | Notes |
|-----------|---------|--------|-------|
| `input` | `InputExecutor` | ✅ | tested end-to-end; supports `param_name` + fallback `value`/`default_value` |
| `prompt` | `PromptExecutor` | ✅ | tested end-to-end |
| `output` | `OutputExecutor` | ✅ | keys results by `config.output_name` |
| `log` | `LogExecutor` | ✅ | tested end-to-end |
| `merge` | `MergeExecutor` | ✅ | `concat` strategy; `interleave` is future |
| `loop_start`/`loop_end` | `LoopEngine` | ✅ | unit + end-to-end tested with feedback/loop_count |
| `agent` | `daedalus::Agent` | ✅ **live-verified** | single + multi-agent runs complete against MiniMax-M2.7 |

There is intentionally **no `llm` node**. HiveMind runs agents; one-shot
LLM calls are just agents without tool use. daedalus already provides the
full agent loop, so a separate plain-LLM node would be redundant.

## What to do next

Remaining, in priority order:

1. **Webhook trigger node** (first slice of the trigger roadmap below)
   - Smallest trigger: `POST /api/hooks/:graph_id/:secret` → run inputs →
     existing run path. No scheduler thread required.

## What *not* to do yet

- Don't add WebSocket support — v1 is HTTP polling (`GET /api/runs/:rid/output`).
- Don't add more node types (`if`, `filter`, `map`, etc.) until triggers land.

## How to pick up

```bash
cd /workspace/AI/Hive-Mind
cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure
./build/hivemind --port 8421
```


## Agent node Inspector — ✅ DONE (2026-08-07)

Shipped and browser-verified:

- **Backend:** `GET /api/agent/skills` + `GET /api/agent/mcp` added (thin
  wrappers over `Agent::list_skills()` / `mcp_configured_servers()`).
  All four introspection endpoints (`/tools`, `/status`, `/skills`, `/mcp`)
  are serialized behind a mutex — concurrent `daedalus::Agent`
  construction races on shared global state (was a SIGSEGV when the
  browser fired them simultaneously).
- **Executor:** `AgentExecutor` reads per-node `auto_approve`,
  `mcp_enabled`, and the disable-lists `tools_disabled[]`,
  `skills_disabled[]`, `mcp_servers_disabled[]`, mapped onto
  `daedalus::AgentConfig`.
- **Frontend:** Agent inspector renders model dropdown (grouped by
  provider, `/api/models`), project root, system prompt, auto-approve +
  MCP checkboxes, and collapsible Tools / Skills / MCP checkbox groups
  fed by the `AgentMeta` cache. Disable-list round-trips verified in the
  browser (uncheck `tell_user` → `tools_disabled: ["tell_user"]`;
  recheck → `[]`).

Agent config shape (what `AgentExecutor` consumes): `model`, `project`,
`system_prompt`, `auto_approve`, `mcp_enabled`, `tools_disabled[]`,
`skills_disabled[]`, `mcp_servers_disabled[]`.

## Done milestones (kept for history)

- ~~GraphRunner end-to-end CTest~~ ✅ 2026-08-08 — `test_graph_runner.cpp`:
  merge flow, loop body runs exactly 3×, parallel fan-out; asserts on the
  event stream + persisted results with an isolated temp-HOME DB. 4/4 ctest.

- ~~Live `agent` node run~~ ✅ 2026-08-07 — single-agent and 3-agent
  fan-out/merge/fan-out runs complete against MiniMax-M2.7 (~40s for the
  multi-agent DAG). Unknown models fail fast with a clear error.
- ~~Workflow save/load files~~ ✅ 2026-08-07 — `.hivemind.json`
  export/import with id remapping + UI buttons and workflow switcher.
- ~~Agent node directory browser~~ ✅ 2026-08-07 — `/api/fs/browse` +
  modal picker on the Agent inspector's project field.
- ~~New-workflow button~~ ✅ 2026-08-08 — toolbar New → name prompt →
  empty backend graph.
- ~~UI panel system polish~~ ✅ 2026-08-07/08 — explicit grid placement,
  restore strips, dark native widgets, drag/wire fixes.


## Future roadmap (big-ticket items — discussed 2026-08-08)

Ideas from the user; not scheduled yet, order TBD. Capture scope notes here
as they firm up.

### 1. User logins / auth
- Server is currently single-user, no auth at all (local trust model).
- Options: simple session-token login (SQLite `users` table, cookie),
  or defer and gate behind a reverse proxy (nginx/traefik basic auth).
- Multi-user raises: per-user graph namespaces, per-user run history,
  per-user model/credentials access (today everything shares the daedalus
  SettingsStore — fine single-user, needs scoping for multi-user).
- Touch points: `main.cpp` (auth middleware on routes), `GraphStore`
  (add `owner` column to graphs/runs), UI login screen + session cookie.

### 2. Trigger nodes (auto-fire workflows)
- New node category: workflow starts from an event instead of ▶ Run.
- Candidate trigger types:
  - **cron/schedule** — "run this graph every N minutes / at 09:00 daily".
    Needs a scheduler thread in `main.cpp` that scans registered triggers
    and calls the same run path as `POST /api/graphs/:gid/run`.
  - **webhook** — `POST /api/hooks/:graph_id/:secret` fires the graph;
    body becomes run inputs. Cheap to build on existing run plumbing.
  - **file watch** — inotify on a directory; new/changed file path becomes
    an input. Needs `std::filesystem` watcher thread.
  - **another run completes** — graph A finishing triggers graph B
    (chaining). Natural fit with the event stream.
- Common design: a `trigger` node type whose executor is a no-op (like
  `input`), plus a `TriggerRegistry` that owns the firing side. The
  runner doesn't need to change — triggers just inject `run_inputs` and
  start a run.
- Storage: `triggers` table in the HiveMind DB (graph_id, kind, config,
  enabled, last_fired_at).

### 3. Other "lots to do" candidates (from earlier discussions)
- WebSocket `/ws/runs` live streaming (replace HTTP polling).
- GraphRunner end-to-end CTest (in-memory run, no server).
- Merge `interleave` strategy.
- Graph versioning / multi-graph (subgraph) nodes.
- Per-node approval UI for agent shell commands.

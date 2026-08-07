# HiveMind — Next Steps / Working Notes

> Status: 2026-08-06 — `llm` node removed (never wanted; the `agent` node covers
> all LLM work via daedalus). All remaining node types are implemented and
> unit-tested. Next milestone: GraphRunner end-to-end CTest, then a live
> `agent` node run.

## Quick project state

- **Build:** `build/hivemind` compiles cleanly.
- **Tests:** `ctest --test-dir build` → 3/3 pass (LoopEngine, node ports, daedalus smoke).
- **Server:** not running; start with `./build/hivemind --port 8421`.
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
| `agent` | `daedalus::Agent` | ✅ inspector done | executor wired; needs a live provider run to verify end-to-end |

There is intentionally **no `llm` node**. HiveMind runs agents; one-shot
LLM calls are just agents without tool use. daedalus already provides the
full agent loop, so a separate plain-LLM node would be redundant.

## What to do next

1. **Add a GraphRunner end-to-end CTest** (no API key needed)
   - Construct a graph in memory and call `runner.run_sync()` without a server.
   - Covers: `input → prompt → merge → output` and a separate loop graph.
   - Catches regressions in the runner's parallel dispatch and loop logic.

2. **Live `agent` node run** (needs an API key)
   - The daedalus SettingsStore (`~/.local/share/daedalus/daedalus.db`)
     already has `minimax` and `moonshot` configured with keys
     (see `GET /api/models`).
   - Run the sample multi-agent DAG from the UI
     (or a single `prompt → agent → output` graph via curl).
   - Verify `node_output` streaming events and the final result.

## What *not* to do yet

- Don't touch the UI beyond the node chrome already built.
- Don't add WebSocket support — v1 is HTTP polling (`GET /api/runs/:rid/output`).
- Don't add more node types (`if`, `filter`, `map`, etc.) until the current set is solid.

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

## What remains

1. **Live `agent` node run** — the final milestone. Needs
   `MINIMAX_API_KEY` (or moonshot) exported. Run the sample
   multi-agent DAG or a single `prompt → agent → output` graph and
   verify `node_output` events + final result.
2. **GraphRunner end-to-end CTest** — in-memory graph run without a
   server (`input → prompt → merge → output`, plus a loop graph).
3. (Later) WebSocket `/ws/runs` instead of HTTP polling.

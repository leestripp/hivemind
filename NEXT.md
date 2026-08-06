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
| `agent` | `daedalus::Agent` | ⏸️ | final milestone; needs a live provider |

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


## Agent node Inspector — the big one (LAST)

The Agent editor is not a few text fields — it's a multi-section panel
with selectors fed by live backend data. `FIELD_SCHEMAS` already supports
`text / textarea / number / select / display`; the Agent work adds
**async-populated selects** and **multi-select checkbox groups**, plus a
`model picker` that groups by provider.

### Planned layout

```
Name            [ Greeter                    ]
Provider/Model  [ minimax — MiniMax-M3     ▼ ]   grouped by provider, from GET /api/models
Project root    [ /path/to/project           ]   working dir for tools
System prompt   [ (optional override)        ]   textarea
Auto-approve    [x] run shell commands without asking
Tools           [x] execute_bash  [x] read_file  [ ] create_file …   checkbox group, from GET /api/agent/tools
Skills          [x] code-review   [ ] git-workflow …                 checkbox group, from GET /api/agent/skills  (NEW endpoint)
MCP servers     [x] filesystem    [ ] github …                       checkbox group, from GET /api/agent/mcp     (NEW endpoint)
```

### Backend work required

| Endpoint | Status | Backed by |
|----------|--------|-----------|
| `GET /api/models` | ✅ live (2 models: minimax, moonshot) | `SettingsStore::list_models/resolve_model` |
| `GET /api/agent/tools` | ✅ live (27 tools) | `Agent::list_tools()` |
| `GET /api/agent/skills` | ❌ add | `Agent::list_skills()` exists in daedalus |
| `GET /api/agent/mcp` | ❌ add | `Agent::mcp_configured_servers()` exists in daedalus |

Both missing endpoints are thin wrappers in `AgentRoutes.cpp` using the
existing `introspection_agent_config()` pattern.

### Frontend work required

1. Extend `FIELD_SCHEMAS` with `kind: 'multiselect'` (checkbox group) and
   async select population (`source: '/api/models'` fetched on inspector open).
2. Agent config shape (what `AgentExecutor` must learn to read):
   `model`, `project`, `system_prompt`, `auto_approve`,
   `enabled_tools[]`, `enabled_skills[]`, `enabled_mcp[]`.
3. Executor support: daedalus `AgentConfig` needs the tool/skill/MCP
   allow-lists mapped — check daedalus API for per-agent filtering
   (may need `disabled_tools` or per-tool registration; verify against
   `AgentConfig` fields before writing UI).
4. Keep raw JSON `<details>` as fallback throughout.

### Sequencing (when we start)

1. Backend: add `/api/agent/skills` + `/api/agent/mcp` (small).
2. Verify daedalus-side allow-list mechanism for tools/skills/MCP.
3. Model/provider dropdown slice (highest value, endpoints ready).
4. Project/system-prompt/auto-approve slice (plain fields, easy).
5. Tools/skills/MCP multi-selects (needs #2).

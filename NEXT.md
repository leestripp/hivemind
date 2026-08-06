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

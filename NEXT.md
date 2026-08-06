# HiveMind — Next Steps / Working Notes

> Status: 2026-08-06 — working tree is clean, `daedalus` migration is committed, all
> non-Agent node types are implemented and unit-tested. Next milestone is a live
> `llm` node end-to-end, then a GraphRunner CTest, then the `agent` node.

## Quick project state

- **Build:** `build/hivemind` exists and compiles cleanly.
- **Tests:** `ctest --test-dir build` → 3/3 pass (LoopEngine, node ports, daedalus smoke).
- **Server:** not currently running; can start with `./build/hivemind --port 8421`.
- **Git:** clean working tree on `main` (one big initial commit containing the full migration).

## Node-type status

| Node type | Backend | Status | Notes |
|-----------|---------|--------|-------|
| `input` | `InputExecutor` | ✅ | tested end-to-end; supports `param_name` + fallback `value`/`default_value` |
| `prompt` | `PromptExecutor` | ✅ | tested end-to-end |
| `output` | `OutputExecutor` | ✅ | keys results by `config.output_name` (done) |
| `log` | `LogExecutor` | ✅ | tested end-to-end |
| `merge` | `MergeExecutor` | ✅ | `concat` strategy; `interleave` is future |
| `loop_start`/`loop_end` | `LoopEngine` | ✅ | unit + end-to-end tested with feedback/loop_count |
| `llm` | `LLMExecutor` / libcurl | ⚠️ | compile-tested; needs a real model/API key to verify live |
| `agent` | `daedalus::Agent` | ⏸️ | intentionally deferred; do it last |

## What to do next

1. **Test the `llm` node end-to-end** (top priority)
   - Get a model configured in `~/.codemax/settings/settings.json` and an API key in env.
   - Create a tiny graph: `input` → `llm` → `output` (or `prompt` → `llm` → `output`).
   - Run it and confirm a model response lands in the output.
   - This validates `LLMClient`, `LLMSettings`, and the model-config pipeline.

2. **Add a GraphRunner end-to-end CTest**
   - Construct a graph in memory and call `runner.run_sync()` without a server.
   - Covers: `input → prompt → merge → output` and a separate loop graph.
   - Catches regressions in the runner's parallel dispatch and loop logic.

3. **Return to the `agent` node** (only after 1 and 2 are green)
   - It already uses `daedalus::Agent` + `CapturingSink` + `LLMSettings`.
   - Needs a live provider and likely some UI tweaks to display multi-agent streams.
   - The sample multi-agent DAG in the UI is waiting for this.

## What *not* to do yet

- Don't touch the UI beyond the node chrome already built — the front-end can render
  nodes/edges but we haven't wired event-stream rendering for live tokens.
- Don't add WebSocket support — v1 is HTTP polling (`GET /api/runs/:rid/output`).
- Don't add more node types (`if`, `filter`, `map`, etc.) until the current set is solid.

## How to pick up the next task

Run the server and try a live `llm` graph:

```bash
cd /workspace/AI/Hive-Mind
./build/hivemind --port 8421

# In another shell, create a graph, add nodes/edges, and run it.
# Or use the web UI at http://localhost:8421/.
```

If no API key is available, the next best thing is the GraphRunner CTest in step 2.

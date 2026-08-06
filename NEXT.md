# HiveMind — Next Steps / Working Notes

> Status: 2026-08-05 — new Loop node pair implemented and verified. Next: test `llm` node and fix `OutputExecutor` result key.

## Decision: Agent node is last

The `agent` node delegates to `daedalus::Agent`, which requires:
- A configured LLM provider (`~/.codemax/settings/settings.json`).
- A valid `MINIMAX_API_KEY` / `OPENAI_API_KEY` / etc. in the environment.
- Network access to the provider's chat-completions endpoint.
- Non-trivial runtime (tool calls, MCP servers, skills, context compression).

Because the Agent node depends on all of that, **leave it for the end**.
Get the simpler node types rock-solid first, then return to Agent as the
final integration step.

## Non-Agent node checklist

| Node type | Backend | Status | Needs work |
|-------------|---------|--------|------------|
| `input`     | `InputExecutor`        | ✅ tested end-to-end | — |
| `prompt`    | `PromptExecutor`       | ✅ tested end-to-end | — |
| `output`    | `OutputExecutor`       | ✅ tested end-to-end | Minor: result key currently uses empty port name (`node_id:`) instead of `output_name` |
| `log`       | `LogExecutor`          | ✅ tested end-to-end | — |
| `merge`     | `MergeExecutor`        | ✅ tested end-to-end | Only `concat` strategy implemented; `interleave` is future |
| `llm`       | `LLMExecutor` / libcurl | ⚠️ compile-only | Needs API key + model config to test live |
| `loop_start` | `LoopStartExecutor` / `LoopEngine` | ✅ end-to-end tested | — |
| `loop_end`   | `LoopEndExecutor` / `LoopEngine`   | ✅ end-to-end tested | — |
| `agent`     | `daedalus::Agent`      | ⏸️ deferred | Do last |

## Immediate next tasks (suggested order)

1. **Fix `OutputExecutor` result key** (next)
   - Current: `results["node_id:out"]` (port is empty, so it becomes `node_id:`).
   - Should be: `results["output_name"]` matching the node's `config.output_name`.
   - See `src/executors/OutputExecutor.cpp` and `src/GraphRunner.cpp`.

2. **Test `llm` node end-to-end**
   - Set a valid API key (e.g. `MINIMAX_API_KEY`) or use `tools/setup-models.sh`.
   - Build a tiny graph: `prompt → llm → output`.
   - Verify it streams/returns the model response.

3. ~~Test loop nodes end-to-end~~ ✅ Done.

4. **Add a GraphRunner end-to-end test**
   - A CTest test that constructs a graph in memory and runs it without a server.
   - Covers `input → prompt → merge → output` at minimum.

5. **Return to `agent` node**
   - Only after items 1-4 are green.
   - Verify provider config, API key, and stream events through the WebSocket/HTTP event stream.

## Notes

- The `agent` node's plumbing is already in place (`daedalus::Agent`, `CapturingSink`, `AgentConfig` from `LLMSettings`). It just needs a live provider to exercise it.
- The UI already loads a sample multi-agent DAG, but clicking **Run** on it will fail until a provider key is configured. For testing non-Agent flows, build graphs that don't contain `AI Agent` nodes.
- WebSocket streaming (`/ws/runs`) is documented but not yet implemented; v1 uses HTTP polling via `GET /api/runs/:rid/output`. That's fine for non-Agent testing.

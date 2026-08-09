# workflows/ — canonical test workflows

Importable `.hivemind.json` workflow files (see `POST /api/graphs/import`
in `docs/api.md`). Use them as smoke tests for agents driving HiveMind.

## agent-loop-demo.hivemind.json

Agent-judged refinement loop — the canonical `agent_loop_start` /
`agent_loop_end` example:

```
topic(input) → loop start → writer(agent) → judge(agent_loop_end) → final tagline(output)
                    ↑              │              ▲
                    └── judge feedback (continue) ┘
              judge instructions (prompt) → judge.prompt_in
              loop start.loop_count → judge.loop_count
```

- **writer** drafts/improves a one-sentence tagline (MiniMax-M2.7).
- **judge** critiques each draft and calls `loop_decision`:
  `continue` + one specific improvement, or `exit` + the final tagline.
- `max_iterations: 4` caps the loop.

Test it:

```bash
# import (creates a NEW graph with remapped ids)
curl -X POST http://localhost:8421/api/graphs/import \
  -H 'Content-Type: application/json' \
  -d @workflows/agent-loop-demo.hivemind.json

# run the imported graph, then poll until finished
GID=<imported graph id>
RID=$(curl -s -X POST http://localhost:8421/api/graphs/$GID/run -d '{}' | jq -r .run_id)
curl -s http://localhost:8421/api/runs/$RID/output
```

Expected: `loop_iteration` events (judge said continue), then
`graph_completed` with `results.tagline` = the final tagline. Requires a
configured MiniMax-M2.7 model (daedalus SettingsStore).

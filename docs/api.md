# HiveMind REST API

> Base URL: `http://localhost:8421` (default; `--port N` or `HIVEMIND_PORT` to change).
> All bodies are JSON (`Content-Type: application/json`). Errors return
> `4xx/5xx` with `{"error": "..."}` where applicable.
> No authentication — intended for local use. Everything an external agent
> needs to build, run, and schedule workflows is here.

## Conventions

- IDs are UUID v4 strings, generated server-side on creation.
- Node **config** is a free-form JSON object whose shape depends on the node
  `type` (see [Node types](#node-types)).
- Times are server-local ISO-8601 (`YYYY-MM-DDTHH:MM:SS`) for schedules;
  UTC (`…Z`) for graph metadata.
- Live run output is **polled** (`GET /api/runs/:rid/output`); WebSocket
  streaming is not yet implemented.

---

## Health & discovery

### `GET /api/health`
Liveness probe.
```json
{ "status": "ok", "service": "hivemind" }
```

### `GET /api/node-types`
Registered node types with port shapes.
```json
{ "types": ["input", "prompt", "agent", "merge", "loop_start", "loop_end", "output", "log"] }
```

### `GET /api/models`
List models from the daedalus SettingsStore (the only source of truth for
providers/credentials — HiveMind stores none).
```json
[{ "name": "MiniMax-M2.7", "provider": "MiniMax", "host": "https://api.minimax.io",
   "has_key": true, "is_default": true, "max_context_size": 200000 }]
```

### `GET /api/models/:name`
Single model by name (404 if unknown).

### `GET /api/agent/tools`
Tools a `daedalus::Agent` exposes to `agent` nodes (27 built-ins).
```json
[{ "name": "execute_bash", "description": "…" }]
```

### `GET /api/agent/skills`
Discoverable skills: `[{ "name", "description", "tags" }]`.

### `GET /api/agent/mcp`
Configured MCP server names: `["minimax-web-search", "wikipedia"]`.

### `GET /api/agent/status`
Agent engine info: `{ "status": "ok", "tool_count": 27, "tools": [...], "stats": {...} }`.

### `GET /api/projects`
Known project roots (scanned from codemax + local dirs).

### `GET /api/projects/:name`
Single project entry (404 if unknown).

### `GET /api/fs/browse?path=/some/dir`
Directory listing for the Agent-node project-root picker.
```json
{ "path": "/home/u", "parent": "/home", "dirs": ["src", "docs"], "files": ["a.txt"] }
```

---

## Graphs (workflows)

### `GET /api/graphs`
List all graphs (metadata only, no nodes/edges).
```json
[{ "id": "uuid", "name": "canvas-session", "description": "…",
   "created_at": "…Z", "updated_at": "…Z" }]
```

### `POST /api/graphs`
Create a graph. Body: `{ "name": "…", "description": "…" }` → 201 + graph.

### `GET /api/graphs/:id`
Full graph: metadata + `nodes` + `edges`.
```json
{ "id": "uuid", "name": "…", "description": "…",
  "nodes":  [{ "id", "type", "label", "config": {...}, "position_x": 40, "position_y": 100 }],
  "edges":  [{ "id", "source_node", "source_port", "target_node", "target_port" }] }
```

### `PUT /api/graphs/:id`
Update metadata (name/description).

### `DELETE /api/graphs/:id`
Delete graph + nodes + edges (cascade).

### `GET /api/graphs/:id/export`
Portable workflow file (`.hivemind.json`, version 1). Deterministic key
order — diffs cleanly in git.

### `POST /api/graphs/import`
Import an exported file (body = the export JSON). Validates `version`,
remaps node IDs to fresh UUIDs, always creates a **new** graph (never
overwrites). Unknown node types are rejected.

---

## Nodes & edges

### `POST /api/graphs/:gid/nodes`
Add a node. Body:
```json
{ "type": "prompt", "label": "ask", "config": { "value": "…" },
  "position_x": 100, "position_y": 200 }
```
→ 201 + created node (server assigns `id`).

### `PUT /api/graphs/:gid/nodes/:nid`
Update node `label`, `config`, and/or position.

### `DELETE /api/graphs/:gid/nodes/:nid`
Delete node + its connected edges.

### `POST /api/graphs/:gid/edges`
Connect ports. Body:
```json
{ "source_node": "uuid", "source_port": "out",
  "target_node": "uuid", "target_port": "in" }
```
→ 201 + created edge.

### `DELETE /api/graphs/:gid/edges/:eid`
Delete an edge.

---

## Runs

### `POST /api/graphs/:gid/run`
Start a run. Returns **202 immediately**; execution happens in the
background. Optional body provides values for `input` nodes (keyed by the
node's `config.param_name`):
```json
{ "inputs": { "topic": "Should we use SQLite or Postgres?" } }
```
Response:
```json
{ "run_id": "uuid", "graph_id": "uuid", "status": "running",
  "output_url": "/api/runs/<run_id>/output" }
```

### `GET /api/runs/:rid/output`
Poll the live event log (JSONL-ish text: one `{event:…}` JSON object per
line) + status. Events: `graph_started`, `node_started`, `node_output`,
`node_completed`, `node_failed`, `loop_iteration`, `graph_completed`,
`graph_failed`, `graph_cancelled`.
```json
{ "run_id": "uuid", "status": "running", "finished": false,
  "error": "", "output": "{\"event\":\"graph_started\",…}\n…" }
```
Poll until `"finished": true` (`status` ∈ `completed | failed | cancelled`).

### `GET /api/graphs/:gid/runs/:rid`
Final stored state: status, error, `results` (keyed by `node_id:port`, or
by `output_name` for `output` nodes), `completed_nodes`.

---

## Triggers (scheduled + webhook)

Triggers fire a workflow: scheduled ones via the built-in 30-second-tick
scheduler, webhooks via external HTTP POST. See `docs/design.md` for the
schedule semantics (server-local time, aligned interval ticks, missed
occurrences are skipped — no catch-up storms).

### `GET /api/triggers`
All triggers across all workflows, with `graph_name` joined in.
```json
[{ "id", "graph_id", "graph_name", "kind": "schedule|webhook", "enabled": true,
   "description": "Daily at 09:00", "next_fire_at": "2026-08-09T09:00:00",
   "last_fired_at": "", "created_at": "…",
   "schedule": { "type": "daily", "time": "09:00" },   // schedule kind only
   "inputs": { "topic": "…" },                          // static run inputs
   "secret": "…", "url": "/api/hooks/<gid>/<secret>" }] // webhook kind only
```

### `GET /api/graphs/:gid/triggers`
Triggers for one graph.

### `POST /api/graphs/:gid/triggers`
Create. Scheduled:
```json
{ "kind": "schedule",
  "schedule": { "type": "weekly", "days": [1,3,5], "time": "07:30" },
  "inputs":   { "topic": "morning brief" } }
```
`schedule.type` ∈ `once` (`{"at":"YYYY-MM-DDTHH:MM"}`) · `daily`
(`{"time":"HH:MM"}`) · `weekly` (`{"days":[0-6],"time":"HH:MM"}`, 0=Sun) ·
`interval` (`{"every_minutes":N}`). `once` triggers disable themselves
after firing.

Webhook:
```json
{ "kind": "webhook" }
```
→ 201 + trigger (includes `secret` + `url` for webhooks; `next_fire_at`
for schedules). Invalid schedules → 400 with a reason.

### `PUT /api/triggers/:id`
Edit schedule and/or enable flag:
```json
{ "schedule": { "type": "daily", "time": "08:00" }, "inputs": {…},
  "enabled": false }
```
Recomputes `next_fire_at`. 404 on unknown id.

### `DELETE /api/triggers/:id`
Delete a trigger.

### `POST /api/hooks/:gid/:secret`
**Fire a webhook trigger.** No auth header — the secret in the path IS the
credential. Body becomes the run's `inputs` (any JSON object of
string→string). 404 on wrong secret/graph.
```bash
curl -X POST http://localhost:8421/api/hooks/<gid>/<secret> \
     -H 'Content-Type: application/json' -d '{"topic":"deploy finished"}'
```

---

## Node types

| Type | Inputs | Outputs | `config` keys |
|------|--------|---------|---------------|
| `input` | — | `out` | `param_name` (run-input key), `value`/`default_value` (fallback) |
| `prompt` | — | `out` | `value` (static text) |
| `agent` | `in` | `out` | `model`, `project`, `system_prompt`, `auto_approve`, `mcp_enabled`, `tools_disabled[]`, `skills_disabled[]`, `mcp_servers_disabled[]` |
| `merge` | `in_a`,`in_b`,`in_c` | `out` | `strategy:"concat"`, `separator` (default `"\n\n"`) |
| `loop_start` | `in` | `out`,`loop_count` | `iteration_count` |
| `loop_end` | `in`,`loop_count` | `out` | — |
| `output` | `in` | — | `output_name` (result key) |
| `log` | `in` | — | — |

The `agent` node embeds `daedalus::Agent`; model names come from
`GET /api/models` (daedalus SettingsStore at
`~/.local/share/daedalus/daedalus.db`). Unknown model → the run fails fast
with a clear error.

---

## Static UI

### `GET /`, `GET /*`
Serves the web UI from `web/dist/` (custom handler, always
`Cache-Control: no-cache, no-store, must-revalidate`). The `/*` catch-all
is registered last — all `/api/*` routes take precedence.

---

## Example: build + run a workflow from an external agent

```bash
BASE=http://localhost:8421

# 1. Create graph
GID=$(curl -s -X POST $BASE/api/graphs -d '{"name":"daily-brief"}' | jq -r .id)

# 2. Add nodes
IN=$(curl  -s -X POST $BASE/api/graphs/$GID/nodes -d '{"type":"input","label":"topic","config":{"param_name":"topic"}}' | jq -r .id)
AG=$(curl  -s -X POST $BASE/api/graphs/$GID/nodes -d '{"type":"agent","label":"writer","config":{"model":"MiniMax-M2.7"}}' | jq -r .id)
OUT=$(curl -s -X POST $BASE/api/graphs/$GID/nodes -d '{"type":"output","label":"brief","config":{"output_name":"brief"}}' | jq -r .id)

# 3. Wire
curl -s -X POST $BASE/api/graphs/$GID/edges -d "{\"source_node\":\"$IN\",\"source_port\":\"out\",\"target_node\":\"$AG\",\"target_port\":\"in\"}"
curl -s -X POST $BASE/api/graphs/$GID/edges -d "{\"source_node\":\"$AG\",\"source_port\":\"out\",\"target_node\":\"$OUT\",\"target_port\":\"in\"}"

# 4. Schedule it every weekday at 07:30
curl -s -X POST $BASE/api/graphs/$GID/triggers -d '{"kind":"schedule","schedule":{"type":"weekly","days":[1,2,3,4,5],"time":"07:30"},"inputs":{"topic":"overnight news summary"}}'

# 5. Or run it now
RID=$(curl -s -X POST $BASE/api/graphs/$GID/run -d '{"inputs":{"topic":"test"}}' | jq -r .run_id)
curl -s $BASE/api/runs/$RID/output   # poll until "finished": true
```

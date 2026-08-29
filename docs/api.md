# hive-server API (v2)

The hub is a comms router. Its API serves the client (GTK app and web UI)
and the workers' control plane. All endpoints require a Bearer API key
except `/api/health`.

## Threads & messages (unchanged shape)

| Endpoint | Notes |
|----------|-------|
| `GET /api/threads` / `POST /api/threads` | list / create (agent_ids + title) |
| `GET /api/threads/:id/messages?after=` | incremental poll |
| `POST /api/threads/:id/messages` | user message → hub routes a `task` to each agent participant's worker |
| `GET /api/threads/:id/activity` | live turn state (streamed deltas, busy flags) — fed by worker `task_delta` events |
| `POST /api/threads/:id/attachments` | user upload into the shared attachment store |
| `GET /api/files/download?path=` | attachment/file download (image MIME inline — Phase 34) |

## Roster

| Endpoint | Notes |
|----------|-------|
| `GET /api/agents` | list; includes `worker` placement + live status |
| `POST /api/agents` | create (name, role, model, worker) |
| `PATCH /api/agents/:id` | role/model/worker/mcp_servers/skills edits |
| `DELETE /api/agents/:id` | remove (worker reaps the container) |
| `GET /api/agents/:id/activity` | per-agent live state |

Moving an agent to another worker = PATCH worker; the hub re-homes the
agent (old worker stops it, new worker spawns it, history volume is
worker-local so cross-worker moves start fresh unless migrated manually).

## Workers

| Endpoint | Notes |
|----------|-------|
| `POST /api/workers/enroll` | issue a worker key |
| `GET /api/workers` | list with online status + per-agent liveness |
| `DELETE /api/workers/:id` | decommission |

Worker channel: `wss://…/ws/worker` — ops: task / task_delta /
task_result / approval_req / approval_resp / agent_status / file_fetch /
file_push / skill_sync / agent_spawn / agent_stop / heartbeat.

## Skills

| Endpoint | Notes |
|----------|-------|
| `GET /api/skills` | the hub's canonical library |
| `POST /api/skills/install` | install from registry → syncs to all workers |
| per-agent enable | roster PATCH; applied to containers at next spawn |

## Approvals

Approvals originate INSIDE agents now (daedalus approval hooks) and relay
through the hub: `GET /api/approvals?status=pending`,
`POST /api/approvals/:id/resolve`. The hub holds no tool proxy of its
own.

## Models & credentials

Gone from the hub in v2. Model/provider config lives on each WORKER
(local LLM store) — the client edits it via the hub relaying to the
worker (`/api/workers/:id/models` …) or directly on the worker host.

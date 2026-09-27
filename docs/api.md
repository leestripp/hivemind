# hive-server API

The hub is a comms router. Its API serves the client (GTK app and web UI)
and the workers' control plane. All endpoints require a Bearer API key
except `/api/health`.

## Threads & messages (unchanged shape)

| Endpoint | Notes |
|----------|-------|
| `GET /api/threads` / `POST /api/threads` | list / create (agent_ids + title) |
| `GET /api/deliveries?agent=&state=&limit=` | durable delivery queue (Phase 66): every agent wake/relay with state (`queued`/`dispatched`/`done`/`error`/`dropped`/`expired`), `read` receipt, timestamps and error text — the message-loss diagnosis surface |
| `GET /api/threads/:id/messages?after=` | incremental poll |
| `POST /api/threads/:id/messages` | user message → hub routes a `task` to each agent participant's worker |
| `GET /api/threads/:id/activity` | live turn state (streamed deltas, busy flags) — fed by worker `task_delta` events |
| `POST /api/threads/:id/cancel` | cancel in-flight turns + mute agent fan-in on the thread (~10 min) — the right-click Stop path |
| `POST /api/threads/:id/attachments` | user upload into the shared attachment store |
| `GET /api/files/download?path=[&worker=]` | attachment/file download (image MIME inline — Phase 34); `worker` routes to a worker-hosted store (Phase 35 agent attachments) |

Message records carry an `attachments` array. Two origins: user uploads
(above) and agent `attach_file` tool calls — the worker sweeps
`/workspace/attachments/<thread>/outgoing/` at turn end and the hub
stores the records with the reply message (injecting the worker name).

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
| `GET /api/workers` | list with online status + last_seen |
| `DELETE /api/workers/:name` | decommission |
| `POST /api/workers/:name/default-model` | set worker's default model (`{model}`) |
| `GET /api/workers/:name/models` | full model registry of that worker (**admin**) |
| `POST /api/workers/:name/models` | upsert a model entry `{name, provider, host, …}` (**admin**) |
| `DELETE /api/workers/:name/models/:model` | remove an entry (refuses the default) (**admin**) |
| `POST /api/workers/:name/secrets` | write-only `{provider, key}` → worker's secrets.json (**admin**) |

Model-registry management routes are **relay-only**: the hub forwards
them over the worker channel and persists nothing — models.json /
secrets.json stay worker-local (credentials never live on the hub).

Worker enrollment is **CLI-only** (admin runs it on the hub host):
`hive-server enroll-worker <name>` prints the raw `hmwk_` key ONCE
(SHA-256 hashed in workers.db). The worker then dials OUT:

```
hive-worker --server wss://<server> --key hmwk_... --name <name>
```

Worker channel: **dedicated WebSocket port** (`worker_ws_port`, default
8423), path `/api/workers/channel`. See `docs/hive-worker.md` for the
full op inventory.

## Models & credentials

The hub holds NO LLM config. Each worker owns its local store
(`~/.local/share/hive-worker/models.json` + `secrets.json`, 0600);
credentials are env-injected into agents at spawn (`HIVEMIND_*`).
`GET /api/models` returns the union of connected workers' catalogs
(advertised in their `hello` frames) -- used by the client's New Agent
dialog.

## Skills

| Endpoint | Notes |
|----------|-------|
| `GET /api/skills` | the hub's canonical library |
| `POST /api/skills/install` | install from registry → syncs to all workers |
| per-agent enable | roster PATCH; applied to containers at next spawn |

## Approvals

Approvals originate INSIDE agents now (hive-brain approval hooks) and relay
through the hub: `GET /api/approvals?status=pending`,
`POST /api/approvals/:id/resolve`. The hub holds no tool proxy of its
own.

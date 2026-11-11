# hive-server API

The hub is a comms router. Its API serves the client (GTK app and web UI)
and the workers' control plane. All endpoints require a Bearer API key
except `/api/health`.

## Threads & messages (unchanged shape)

| Endpoint | Notes |
|----------|-------|
| `GET /api/threads` / `POST /api/threads` | list / create (agent_ids + title) |
| `GET /api/deliveries?agent=&state=&limit=` | durable delivery queue (Phase 66): every agent wake/relay with state (`queued`/`dispatched`/`done`/`error`/`dropped`/`expired`), `read` receipt, timestamps and error text — the message-loss diagnosis surface |
| `GET /api/shares?agent=` | agent-to-agent file shares (2026-10-11, admin scope): metadata audit surface for the `share_file` tool — sender/target/size/state (`active`/`revoked`)/fetch counters; blob contents never transit the hub (staged on the sender's worker) |
| `GET /api/threads/:id/messages?after=` | incremental poll |
| `POST /api/threads/:id/messages` | user message → hub routes a `task` to each agent participant's worker. Optional `"source": "user"|"voice"` (message header, Plan.md); anything else → 400 |
| `GET /api/threads/:id/activity` | live turn state (streamed deltas, busy flags) — fed by worker `task_delta` events |
| `POST /api/threads/:id/cancel` | cancel in-flight turns + mute agent fan-in on the thread (~10 min) — the right-click Stop path |
| `GET /api/threads` | each thread carries `unread`: non-user messages newer than the persisted `user` read mark (no mark → all) — the client's badge count, survives restarts. `unread` is gated to user-relevant threads (2026-10-01): kind `user`/kind `dm`, or any group the user has posted in — pure agent-to-agent threads always report 0. Also `group_id`: custom thread-group id or null (= smart group derived from `kind`) |
| `GET /api/thread-groups` | user's custom thread groups (id/name/position/member_count) |
| `POST /api/thread-groups` | create group `{name}` → 201 `{id}` |
| `PATCH /api/thread-groups/:id` | rename `{name}` |
| `DELETE /api/thread-groups/:id` | delete group; members revert to their smart groups (202) |
| `PUT /api/threads/:id/group` | move thread: `{"group_id":"…"}` into a custom group, `null`/absent back to smart. Folders — one group per thread |
| `POST /api/threads/:id/read` | the user viewed the thread → advances ONLY the reserved `user` read mark (202). Never flips agent-addressed delivery receipts — those key off each target agent's own mark; messages addressed to the user (inbox) are the ones this mark governs. Idempotent, monotonic |
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
| `POST /api/agents` | create (name, role, model, worker, memory, cpus) |
| `PATCH /api/agents/:id` | role/model/worker/mcp_servers/skills/memory/cpus edits |

Both also accept `host_agent`; see [host-agents.md](host-agents.md).
The hub returns 409 when the worker lacks the `host_agents` capability.
Runtime selection needs no command-approval or typed-acknowledgment field.
| `DELETE /api/agents/:id` | remove (worker reaps the container); cancels its live turns and deletes **all** agent-authored messages across the User Inbox, DMs, group and pair threads. User, system and other agents’ messages are preserved; the response includes `messages_deleted` |
| `GET /api/agents/:id/activity` | per-agent live state |

Moving an agent to another worker = PATCH worker; the hub re-homes the
agent (old worker stops it, new worker spawns it, history volume is
worker-local so cross-worker moves start fresh unless migrated manually).

Per-agent container resources (Phase 69): `memory` is a podman size
string (`"4g"`, `"1536m"`; `""` = worker default 4g) and `cpus` a core
count (`0` = worker default 2). Both are cgroup CAPS, not reservations —
a 32g cap costs nothing until used. On PATCH, `""`/`-1` = keep current,
`"default"`/`0` = reset to the worker default. The worker applies them
at container create AND reconciles live containers in place via
`podman update` — no recreate needed when limits change.

## Workers

| Endpoint | Notes |
|----------|-------|
| `GET /api/workers` | list with online status + last_seen |
| `DELETE /api/workers/:name` | decommission |
Worker-scoped model/secret management routes and their `model_ctl` relay
were removed after the Phase-43 hub registry became authoritative. They
were not part of task dispatch in the final implementation. Use the hub
provider/model routes below. Old worker-local files are not deleted.

Worker enrollment is **CLI-only** (admin runs it on the hub host):
`hive-server enroll-worker <name>` prints the raw `hmwk_` key ONCE
(SHA-256 hashed in workers.db). The worker then dials OUT:

```
hive-worker --server wss://<server> --key hmwk_... --name <name>
```

Worker channel: **dedicated WebSocket port** (`worker_ws_port`, default
8423), path `/api/workers/channel`. See `docs/hive-worker.md` for the
full op inventory.

## System telemetry

| Endpoint | Notes |
|----------|-------|
| `GET /api/system` | original hub `host`/`now` fallback plus a `workers` array: name, online/last_seen, telemetry availability, and each reporting worker's own `host`/`now` CPU, memory, disk, RSS and container count |
| `GET /api/system/history?minutes=N&worker=name` | one worker's in-memory samples, oldest first; omit `worker` for the original hub series. `minutes` defaults to 60 and is capped at 1440 |
| `GET /api/system/agents` | per-agent exec/message usage for the last 24 h |

Every current worker samples its own Linux host every 15 seconds and sends
`host` + `now` on its existing authenticated heartbeat. The hub retains 24
hours per worker in memory; history resets on hub restart and no extra
connections are opened. Older workers stay online and appear with
`telemetry: false` until upgraded. The System page uses worker sections when
workers are enrolled and the original hub view only as a no-worker fallback.

## Models & credentials

The hub owns the current LLM configuration in
`~/.local/share/hivemind/llm.db`, including provider API keys. Protect
the hub home and backups, and use authenticated TLS worker channels when
workers are off-host. At dispatch the resolved provider configuration is
injected into each task's `llm` object and relayed by the worker to
hive-agent. `GET /api/models` reads this hub registry and supplies model
choices to clients. Blank agent model selects the hub default. Explicit model names on
agent create/patch must resolve in this registry; unknown names are
rejected with HTTP 400 instead of spawning an agent that cannot dispatch.

### Hub model registry (Phase 43 — the authoritative path)

These are hub control-plane endpoints for configuration clients. They are
not tools exposed to a running hive-agent: the hub resolves provider/model
configuration into each task's `llm` block, and the agent cannot change its
own provider/model through these routes. Bearer-scope labels such as
`agent` belong to API credentials, not to the runtime agent's capabilities.


`GET/POST /api/providers`, `GET/POST/DELETE /api/models`,
`POST /api/models/default` manage the hub-owned registry (llm.db); the
resolved provider config is injected into every task frame (`op.llm`).
Model records carry `thinking_level` (2026-10-01): `""` = provider/model
default, else `none|low|high|max`. There is NO provider API that
advertises which thinking levels a model supports (OpenAI's `/v1/models`
returns only id/created/owned_by; OpenAI `reasoning_effort`, Anthropic
thinking budgets and Moonshot `thinking.effort` are all send-time,
provider-specific) — so it is an editor field, validated
`default|none|low|high|max`, and the engine's provider plugin silently
ignores a level the model rejects. `POST /api/agents` / `PATCH
/api/agents/:id` accept an agent-level `thinking_level` override (`""` =
inherit from model, `"default"` = clear, else set); at dispatch the
override wins, else the model's, and the effective value rides
`op.llm.thinking_level` to the agent, which applies it via its runtime
`set_thinking_level`.

`POST /api/providers/:name/discover` fetches `{host}/v1/models` and
populates context/default-temperature metadata from the provider response,
with the hub's Moonshot/MiniMax catalog filling omitted values. This
includes `kimi-k3.1-pro` and `kimi-k3.1-pro-highspeed` (1M context,
fixed temperature 1.0). Re-discovery preserves each model's
`thinking_level` and any previously known metadata the provider omitted.

## Skills

| Endpoint | Notes |
|----------|-------|
| `GET /api/skills/registry` | Curated catalog with installed state |
| `GET /api/skills` | Installed curated starters only |
| `POST /api/skills/install` | `{"name":"catalog-entry"}` → writable workspace copies for all agents |
| `DELETE /api/skills/:name` | Uninstall for all agents; every enrolled worker must be online |

There are no per-agent skill switches. Agents create and edit shared workspace
skills and private skills themselves; the hub does not inventory them. Offline
workers receive missing installed starters automatically on reconnect. Existing
workspace copies are preserved, so reconnects do not undo agent corrections.

## Agent event triggers (worker bridge)

These are agent tools, not public REST endpoints. `create_trigger`,
`list_triggers`, and `delete_trigger` use authenticated
`trigger_req`/`trigger_resp` frames over the agent's worker channel. The only
supported event is currently `agent_idle`.

Create payload: `{action:"add", event:"agent_idle", target:<id-or-name>,
prompt?:<text>}`. The optional prompt is capped at 4000 bytes. List and delete
are owner-scoped; delete takes `{action:"delete", trigger_id:<id>}`.
Registrations live in `triggers.db`, fire once, and are removed after their
normal durable wake is accepted. Idle excludes live turns, queued wakes, and
active managed background processes. An already-idle target fires immediately;
a background completion fires only after its nudge turn also finishes.
See `docs/design.md` for the exact idle and crash-recovery semantics.

## Autonomous execution

Agents execute valid tool calls directly under their configured runtime
identity. There is no command-approval queue or approval-rules API. Legacy
`require_approval`, `auto_approve`, and approval-frame input is inert;
historical approval databases/files are not used to gate execution.

# hive-worker (v2) -- fleet manager + comms router

A hive-worker runs on any Linux machine (including the hive-server host
itself, for always-online agents) and owns everything compute: podman
containers, the agents inside them, and the files they produce.

## Connection

One OUTBOUND WebSocket to the hub's dedicated worker port:

```
hive-worker --server wss://<server>:8423 --key hmwk_... --name <name>
# dials  wss://<server>:8423/api/workers/channel
```

- Enrollment is CLI-only on the hub host: `hive-server enroll-worker
  <name>` prints the raw `hmwk_` key once (SHA-256 hashed in workers.db).
- No inbound ports, no DNS, no TLS setup on the rig -- it dials out.
- Auth: first frame must be `{"op":"hello","worker","key","caps","proto",
  "models"}`; the hub validates the key and replies `welcome` (or `error`
  + close). `models` is the worker's local LLM catalog (from its
  models.json) and feeds the hub's aggregated `GET /api/models`.
- Reconnects with backoff (1s, 2s, 5s, ... capped). On every (re)connect
  the hub re-syncs the skills library and replays `agent_spawn` for every
  roster agent placed on this worker -- a rig can be wiped and rebuilt
  from the hub.

## Channel protocol (WS, JSON text frames)

Hub -> worker:

| op | payload | purpose |
|----|---------|---------|
| `welcome` / `error` | | handshake reply |
| `task` | id, agent_id, thread_id, text | run one agent turn |
| `cancel` | agent_id | abort the in-flight turn |
| `approval_resp` | id, allow | user's approve/deny for a pending tool call |
| `agent_spawn` | agent_id, name, model, system_prompt, auto_approve, mcp_servers | create/ensure container + exec hive-agent (idempotent: "already running") |
| `agent_stop` / `agent_remove` | agent_id | stop exec / reap container + home |
| `exec` | id, agent, argv, env, timeout_s | one-shot command in the agent's container (exec_chunk/exec_done back) |
| `file_fetch` / `file_push` / `file_list` | | move bytes hub<->worker workspace, list dirs |
| `skill_begin` / `skill_file` / `skill_end` / `skill_remove` | | stream the skills library to the worker |
| `screen_ctl` | | start/stop the agent's screen share |
| `stream_open` / `stream_data` / `stream_close` | | bidirectional byte streams (VNC tunnel) |

Worker -> hub:

| op | purpose |
|----|---------|
| `hello`, `heartbeat` | handshake; liveness + per-agent state |
| `agent_status` | ready / error / down per agent |
| `task_delta` | streamed reply text while a turn runs |
| `task_result` | final reply body (or error) for a task |
| `approval_req` | agent asks for human approval (tool call) |
| `agent_msg` | agent->agent message (fan-in at the hub) |
| `agent_log` | anything else the agent emits |
| `exec_chunk` / `exec_done` | exec output / completion |
| `file_chunk` / `file_done` / `file_error` / `file_list_result` / `file_push_ack` | file transfer results |
| `screen_result`, `skill_ack`, `stream_*` | screen control, skill sync, VNC streams |

Transport rules that are LOAD-BEARING (both ends):

- `keepAliveTimeout` must be 0 on the channel server (it is applied as
  SO_RCVTIMEO and inherited by upgraded WS sockets -- silence during a
  long `podman create` otherwise reads as peer-closed).
- Never use receiveText() for protocol frames (per-call buffer drops
  coalesced frames); drive the socket with readLoop() + onText.
- The hub never writes to a worker socket while holding the channel
  mutex: WorkerChannel::send() enqueues onto a per-connection outbound
  queue drained by a dedicated writer thread (a blocking sendText to a
  NAT-dead peer otherwise wedges every other worker's connect).

## Fleet management

The worker owns a container per agent (`hive-<agent-id>`):

- Image: `localhost/hive-agent:v2`, built on the worker from
  `hive-server/data/Containerfile.hive-agent` (checkout-relative; run the
  worker from the repo root) with `--build-arg HIVE_UID=$(geteuid)` so
  the container user matches the host uid.
- Mounts: shared `/workspace` (all agents on this worker collaborate;
  also each agent's daedalus PROJECT ROOT -- state lives at
  `/workspace/.daedalus/daedalus-<Name>/`), per-agent home volume
  (`~/.local/share/hive-worker/home-<agentid>/`), `/skills` read-only
  (synced library at daedalus' global skills location).
- `--userns=keep-id` so container writes land as the worker user.
- Rootless networking is a shared pasta netns -- see docs/containers.md
  for the operational gotchas.
- Liveness: heartbeat reports per-agent state; desired state is replayed
  by the hub on every reconnect.

## Task routing (worker <-> hive-agent, NDJSON over exec stdio)

```
hub --task--> worker --stdin--> hive-agent (podman exec -i, CWD=/workspace)
hub <--ops--- worker <--stdout-- hive-agent
```

hive-agent stdin ops: `task` {id,text,thread_id}, `approval_resp`
{id,allow}, `cancel`, `shutdown` (persists daedalus history, exits 0).

hive-agent stdout events and the channel op the worker maps them to:

| ev | mapped op |
|----|-----------|
| `ready` | `agent_status` state=ready |
| `delta` {task,text} | `task_delta` |
| `message` {task,thread_id,body,error} | `task_result` |
| `approval_request` {id,command} | `approval_req` |
| `send_agent_message` {target,text} | `agent_msg` |
| `status` {state,error} | `agent_status` |
| anything else | `agent_log` |

## LLM config (worker-owned)

`~/.local/share/hive-worker/models.json` (catalog) + `secrets.json`
(provider credentials), both 0600. At spawn the worker renders an env
file (`env/<agent-id>.env`) with HIVEMIND_API_HOST / HIVEMIND_API_KEY /
HIVEMIND_CHAT_ENDPOINT / HIVEMIND_MODEL / HIVEMIND_MAX_CONTEXT /
HIVEMIND_TEMPERATURE and passes it to `podman exec --env-file`. The hub
never sees credentials.

## MCP

MCP server specs arrive in `agent_spawn` (`mcp_servers`) and are seeded
into the agent's daedalus.db by hive-agent at startup; daedalus spawns
them as local child processes inside the container. No tunnels.

## Files

- `file_fetch` / `file_push` / `file_list` ops move bytes between the hub
  and the worker's workspace on explicit request only (file locality).
- Agent artifacts stay on the worker until someone asks for them.

## Skills

The hub streams its canonical library down (`skill_begin` / `skill_file`
/ `skill_end` / `skill_remove`; the worker answers `skill_ack`). The
worker stores it under `~/.local/share/hive-worker/skills/` and
bind-mounts it read-only into containers at daedalus' global skills dir.

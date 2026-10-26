# hive-worker — fleet manager + comms router

A hive-worker runs on any Linux machine (including the hive-server host
itself, for always-online agents) and owns everything compute: podman
containers, the agents inside them, and the files they produce.

## Host agents (Phase 72)

With `--allow-host-agents` (or `HIVE_ALLOW_HOST_AGENTS=1`) the worker
additionally accepts **host agents**: bare-metal `hive-agent` processes
running as a dedicated unix user `hivex-<name>` with passwordless sudo —
NO container, NO sandbox. Machine-destroying power; see
[docs/host-agents.md](host-agents.md) before opting in. Provisioning is
manual and local only: `sudo hive-worker provision-host-agent <name>`
(hostname type-to-confirm; creates user, sudoers entry via `visudo -c`,
installs the binary). The hub checks the worker's advertised
`host_agents` capability. No typed acknowledgment or command approval is
required; host agents execute directly as their provisioned unix user.

Two operational flags round the story out:

- `--host-workspace-root DIR` (env `HIVE_HOST_WORKSPACE_ROOT`) — the
  SHARED workspace every host agent on the rig runs in (cwd = `DIR`
  itself), exactly like the containers' bind-mounted `/workspace`. On
  ubuntu-pc that's the host's `/workspace` RAID mount. It is the
  agents' permanent company drive: **neither roster delete nor
  deprovision ever touches it.** Without the flag the agent runs in its
  home dir instead.
- **Roster delete auto-cleans the unix side.** Deleting a host agent via
  the API makes the worker run the root helper
  (`/usr/local/sbin/hivex-deprovision`, installed by provisioning, with
  a scoped NOPASSWD sudoers grant): processes killed, home archived to
  `/var/backups/hivex-<name>-<timestamp>.tar.gz`, user and sudoers
  entry removed — workspace untouched. If the worker is offline at
  delete time the hub logs an audit line telling you to run
  `sudo hive-worker deprovision-host-agent <name>` manually.

## Running as a service (systemd user unit)

A ready template lives at `hive-server/deploy/hive-worker.service` —
copy it to `~/.config/systemd/user/hive-worker.service`, fill in
`@WORKER_NAME@` / `@WORKER_KEY@` (mint keys on the hub with
`hive-server enroll-worker <name>`), then `systemctl --user
daemon-reload && systemctl --user enable --now hive-worker` and
`sudo loginctl enable-linger $USER`.

Skipping the `enable` is the classic way to make every agent on the rig
vanish after a reboot (droplet, 2026-09-29): the hub is enabled but the
WORKER is not, containers stay stopped, and the roster shows everyone
offline. Post-boot health check:
`systemctl --user is-enabled hive-worker && systemctl --user is-active hive-worker`.

The template sets `KillMode=mixed` on purpose: rootless podman's port
forwarder (pasta) and conmon are descendants of whichever process ran
`podman start`, and the default `control-group` kill mode reaps them on
every service restart — published container ports then refuse
connections (the Phase 54 screen-share bug). Do not remove it.


## Connection

One OUTBOUND WebSocket to the hub's dedicated worker port:

```
hive-worker --server wss://<server> --key hmwk_... --name <name>
# dials  wss://<server>/api/workers/channel  (TLS via the nginx proxy;
# direct ws://<server>:8423 still works on a trusted LAN, and a
# co-hosted worker uses ws://127.0.0.1:8423 — loopback needs no TLS)
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
| `task` | id, agent_id, thread_id, text, roster, user_inbox, disabled_skills, llm | run one agent turn (roster/user_inbox feed the agent's list_agents and read_user_inbox tools) |
| `cancel` | agent_id | abort the in-flight turn |
| `agent_spawn` | agent_id, name, model, system_prompt, mcp_servers, secret_env | create/ensure container + exec hive-agent. Re-spawn of a running agent compares the whole spec: identical = no-op, changed (new secret grants, model, prompt...) = graceful restart with the fresh spec (Phase 65) |
| `agent_stop` / `agent_remove` | agent_id | stop exec / reap container + home |
| `exec` | id, agent, argv, env, timeout_s | one-shot command in the agent's container (exec_chunk/exec_done back) |
| `file_fetch` / `file_push` / `file_list` | | move bytes hub<->worker workspace, list dirs |
| `share_local_copy` / `share_delete` | id, src, dst / path | same-rig share fetch (copy staged blob to `incoming/`, ack via `share_copy_ack`) and revoke cleanup of `shared/<id>/` (2026-10-11 share_file) |
| `skill_begin` / `skill_file` / `skill_end` / `skill_remove` | | stream the skills library to the worker |
| `screen_ctl` | | start/stop the agent's screen share |
| `stream_open` / `stream_data` / `stream_close` | | bidirectional byte streams (VNC tunnel) |

Worker -> hub:

| op | purpose |
|----|---------|
| `hello`, `heartbeat` | handshake; liveness + per-agent state |
| `agent_status` | ready / error / down per agent |
| `spawn_req` | ask the hub to re-push `agent_spawn` (sent after a crash-respawn, which reuses the worker's cached spec — keeps secret grants/model/prompt fresh across restarts) |
| `task_delta` | streamed reply text while a turn runs |
| `task_result` | final reply body (or error) + any attachment records for a task |
| `agent_msg` | agent->agent message (fan-in at the hub) |
| `agent_log` | anything else the agent emits |
| `exec_chunk` / `exec_done` | exec output / completion |
| `file_chunk` / `file_done` / `file_error` / `file_list_result` / `file_push_ack` | file transfer results |
| `share_create_req` / `share_list_req` / `share_fetch_req` / `share_revoke_req` | share_file tool bridge (2026-10-11): the worker stages the blob at `workspace/shared/<uuid>/<name>` (path confinement + 50 MB cap) BEFORE forwarding create; the hub stores metadata only and enforces addressed-share fetch auth |
| `share_copy_ack` | completes a same-worker share fetch (hub relays `share_fetch_resp` to the requester) |
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

- Image chain: `localhost/hive-agent:latest` (base) → `:gpu` (AMD ROCm
  layer) → `:browser` (screen-stack layer: Xvfb/x11vnc/websockify/
  Chromium + `hive-screen.sh`). `:latest` and `:gpu` build from the repo
  ROOT (they COPY `build/hive-agent/hive-agent`); `:browser` builds from
  `hive-server/data` (it COPYs `hive-screen.sh`). Built on the worker
  with `--build-arg HIVE_UID=$(geteuid)` so the container user matches
  the host uid. Non-GPU agents default to `:latest` (the retired `:v2`
  tag is no longer referenced).
- Image drift: when an agent's container is STOPPED and its image id no
  longer matches the target tag (images rebuilt after a dep bump), the
  worker recreates it instead of resurrecting the stale one. Running
  containers are never bounced — they adopt the new image at their next
  natural restart.
- Mounts: shared `/workspace` (all agents on this worker collaborate;
  also each agent's hive-brain PROJECT ROOT -- state lives at
  `/workspace/.hive-agent/<Name>/`), per-agent home volume
  (`~/.local/share/hive-worker/home-<agentid>/`), `/skills` read-only
  (synced library at hive-brain' global skills location).
- `--userns=keep-id` so container writes land as the worker user.
- Rootless networking is a shared pasta netns -- see docs/containers.md
  for the operational gotchas.
- Liveness: heartbeat reports per-agent state; desired state is replayed
  by the hub on every reconnect.
- Idle watcher (60s tick, worker-side, channel-independent):
  * **Screen auto-sleep** — counts ESTABLISHED :5900 conns per container
    (parsed from /proc/net/tcp{,6}, zombie-proof since zombies hold no
    sockets) and runs `hive-screen.sh stop` after
    HIVEMIND_SCREEN_IDLE_MIN (default 15) viewerless minutes.
  * **DNS wedge health check** — probes `timeout 8 getent hosts
    github.com` per container; after 3 consecutive failures, restarts
    the container (fresh netns) when the agent is idle (state "ready" /
    not in fleet). Busy agents get a journal warning and are recycled
    once idle — a busy agent may be doing legit offline work.
  * Gotcha behind both: popen pipes must ALWAYS be drained
    (`podman_simple` does) or the child dies with SIGPIPE (rc 141).
- State semantics: fleet state is EVENT-driven (ready/acting/down from
  the exec's NDJSON stream and the process monitor). A remembered state
  can go stale if a transition is missed -- any completed turn resets it
  to ready. Hub-side, an agent whose worker is OFFLINE always reports
  "down" regardless of the remembered state (HubRouter::agent_state,
  2026-08-31: Bob-on-dev-rig showed "ready" on a powered-off rig).

## Task routing (worker <-> hive-agent, NDJSON over exec stdio)

```
hub --task--> worker --stdin--> hive-agent (podman exec -i, CWD=/workspace)
hub <--ops--- worker <--stdout-- hive-agent
```

hive-agent stdin ops: `task` {id,text,thread_id}, `cancel`, `shutdown`
(persists hive-brain history, exits 0).

hive-agent stdout events and the channel op the worker maps them to:

| ev | mapped op |
|----|-----------|
| `ready` | `agent_status` state=ready |
| `delta` {task,text} | `task_delta` |
| `message` {task,thread_id,body,error} | `task_result` |
| `send_agent_message` {target,text} | `agent_msg` |
| `status` {state,error} | `agent_status` |
| anything else | `agent_log` |

Failure semantics (Phase 65b): the worker tracks every `task` frame
DELIVERED to the exec's stdin until its `message` answer arrives. If the
exec exits with tasks still unanswered — crash or graceful stop alike —
the worker synthesises error `message` events for them, so the hub closes
those turns immediately instead of wedging until the watchdog grace
(cancel unconfirmable, thread queue stranded; the 2026-09-25 PA wedge).
Task frames still queued in `pending_` at `stop()` are failed the same
way rather than dying silently with the process object, and frames whose
stdin write fails (EPIPE) are requeued for the respawn flush instead of
being dropped. Net rule: a dispatched task ALWAYS gets exactly one
`task_result` — from the agent, or from the worker standing in for a
dead one.

## LLM config (worker-owned)

`~/.local/share/hive-worker/models.json` (catalog) + `secrets.json`
(provider credentials), both 0600. At spawn the worker renders an env
file (`env/<agent-id>.env`) with HIVEMIND_API_HOST / HIVEMIND_API_KEY /
HIVEMIND_CHAT_ENDPOINT / HIVEMIND_MODEL / HIVEMIND_MAX_CONTEXT /
HIVEMIND_TEMPERATURE and passes it to `podman exec --env-file`. The hub
never sees credentials at rest; hive-agent sets hive-brain'
`explicit_model_wins` so the per-agent settings store can never override
this injection (model-registry unification Phase 1 — models.json is the
single source of truth).

### Registry management over the channel (`model_ctl`)

The hub relays registry management to the worker (it persists nothing):

| `action` | frame fields | effect |
|---|---|---|
| `set_default` (default) | `model` | writes the models.json `"default"` key |
| `list` | — | replies `data` = `{"models": {name: entry}, "default": name}` (secret-free) |
| `upsert` | `model`, `entry` | create/replace an entry; requires provider + host |
| `delete` | `model` | removes an entry; refuses the current default |
| `set_secret` | `provider`, `key` | write-only into secrets.json (0600 enforced); never echoed |

All reply synchronously as `model_result` (`error` set on failure).
Frames omitting `action` behave as `set_default` (older hubs).

## MCP

MCP server specs arrive in `agent_spawn` (`mcp_servers`) and are seeded
into the agent's hive-agent.db by hive-agent at startup; hive-agent spawns
them as local child processes inside the container. No tunnels.

### Blender MCP (:gpu image, verified 2026-09-07)

Official `projects.blender.org/lab/blender_mcp` (both halves from one
snapshot). Chain: hive-brain spawns the `blender-mcp` stdio bridge → bridge
connects to the addon's TCP socket on localhost:9876 → requests execute
inside Blender. Gotchas:

- **Pin `mcp[cli]<2`** at image build — upstream declares `mcp>=1.2.0`
  unbounded and mcp 2.x renamed FastMCP → MCPServer, killing the bridge.
- The addon registers the `blender_mcp` CLI command in `register()`, so
  the extension must be **enabled** (persisted userpref) before
  `blender --background --command blender_mcp` works. `blender-mcp-up.sh`
  handles enable-once + launch + port-wait; `hive-agent-init.sh` (the
  :gpu CMD) runs it at container start.
- Background mode is synchronous-only (no deferred responses; that needs
  the UI event loop on the Xvfb desktop).
- Tools are namespaced `mcp__blender__*` in hive-brain and do NOT appear in
  the agent's static prompt tool list — agents must be told the exact
  names in their system prompt (Forge's prompt documents this).
- Executed code: `bpy` is NOT pre-imported; return data via the `result`
  variable (top-level `return` is a SyntaxError inside exec).

## Agent service sharing (LAN)

Every agent container gets a **fixed block of host ports** published 1:1
onto its public-service range (container ports 8000+):

    -p 0.0.0.0:<base>-<base+N-1>:8000-<8000+N-1>

An agent that binds a service (web app, API, demo) to a port in its
container range is immediately reachable on the LAN at
`http://<worker-host>:<base + (port - 8000)>` — a **stable URL** that
survives container recreation, image updates and worker restarts. No
tunnels, no port allocation at use time, no recreate to add a service.

- **Allocation**: the base port is allocated once per agent (lowest free)
  and persisted in `<worker_dir>/service_ports.txt`
  (`~/.local/share/hive-worker/service_ports.txt`).
- **Agent-side discovery**: containers get `HIVEMIND_SERVICE_HOST`,
  `HIVEMIND_SERVICE_PORT_BASE`, `HIVEMIND_SERVICE_PORT_COUNT`,
  `HIVEMIND_SERVICE_CONTAINER_BASE` env at create. The agent's
  `share_service(name, port, health_path?)` tool registers the service in
  `/workspace/.hivemind/services.json` (health-checked) and returns the
  URL; `list_services` / `unshare_service` manage the registry.
- **Boot hook**: after every (re)create the worker runs
  `/workspace/.hivemind/boot.sh` (detached, if present+executable) so
  agent-owned services self-recover; output appends to
  `/workspace/.hivemind/boot.log`.
- **Rolling adoption**: containers created before the feature are
  recreated once automatically (PortBindings probe beside
  `lan_ports_present`).

Knobs (worker process env): `HIVE_SERVICE_POOL_BASE` (default 18400),
`HIVE_SERVICE_PORT_COUNT` (default 10; 0 disables),
`HIVE_SERVICE_HOST` (default: first global non-loopback IPv4).

`HIVE_LAN_PUBLISH` remains the manual escape hatch for one-off ports —
keep its specs OUT of the pool range; the registry only tracks its own
pool. LAN-only by design: workers stay outbound-only; public ingress is
a separate phase-2 decision.

## Files

- `file_fetch` / `file_push` / `file_list` ops move bytes between the hub
  and the worker's workspace on explicit request only (file locality).
- Agent artifacts stay on the worker until someone asks for them.
- **Agent → chat attachments** (Phase 35): the agent's `attach_file`
  tool copies a workspace file into
  `/workspace/attachments/<thread_id>/outgoing/`; at turn end the worker
  moves those into the canonical `attachments/<thread_id>/` dir and
  attaches records to `task_result`. The hub injects the worker name
  into each record; clients download with
  `GET /api/files/download?path=…&worker=<name>`.

## Skills

The hub streams its canonical library down (`skill_begin` / `skill_file`
/ `skill_end` / `skill_remove`; the worker answers `skill_ack`). The
worker stores it under `~/.local/share/hive-worker/skills/` and
bind-mounts it read-only into containers at hive-brain' global skills dir.

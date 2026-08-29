# hive-worker — remote container execution (Phase 30 design)

Status: **30.1–30.4 DONE (2026-08-26, in release 0.5.0)** — registry,
channel, exec dispatch, screen proxy all shipped and user-validated
end-to-end (GTK client → server → worker on dev-rig → container reply +
live screen share). Only **30.5 TLS** remains — it gates REAL remote
workers (secrets cross the wire per-exec); localhost/LAN workers are
fine on plain ws:// until then. DONE 2026-08-27: `wss://` works with no
code changes (OmniNet TLS client: system CA + verify-peer + SNI; override
with SSL_CERT_FILE for self-signed). Production topology: nginx terminates
TLS on the droplet (workers.<domain> → 127.0.0.1:8423), server binds
localhost via worker_ws_bind in server.json.

## Goal

Today every agent's container runs on the hive-server machine (the rig)
via rootless podman. hive-worker lets OTHER machines run agent containers
while the central hive-server keeps everything else: roster, threads,
approvals, LLM credentials, scheduling, the exec log, the API.

Scaling story: add a GPU box for a vision agent, a Mac mini for Xcode
work, a spare laptop as burst capacity — all managed from the one server.

## Architecture

```
┌──────────────────────────┐          ┌──────────────────────────┐
│      hive-server (rig)   │          │   hive-worker (remote)   │
│                          │  WSS     │                          │
│  AgentHost (LLM brain) ──┼─────────►│  podman (rootless)       │
│  credentials NEVER leave │ control  │  agent containers        │
│  ThreadStore, approvals  │ channel  │  exec, files, screen     │
│  exec-log (audit trail)  │◄─────────┤  results + stream        │
└──────────────────────────┘          └──────────────────────────┘
```

**The worker dials OUT.** One persistent WebSocket to
`wss://hive-server:8421/api/workers/channel`. No inbound ports on the
worker — NAT/CGNAT friendly. OmniNet's WebSocket has client-side connect
(verified 2026-08-25), so the stack stays OmniNet-only.

## What the worker is

A single C++ binary (reuses ContainerManager almost unchanged — it already
speaks podman, builds images, runs the screen stack). It does NOT link
daedalus, has no LLM keys, no roster DB, no threads. It is a dumb,
audited executor: run this command in container X, stream back output.

## Protocol (JSON text frames over the WS)

Server → worker:
- `{"op":"ensure","agent":"...","image":"...","mounts":[...]}` — create/start container
- `{"op":"exec","id":"<job>","agent":"...","cmd":"...","timeout_s":120}`
- `{"op":"stop"/"remove","agent":"..."}`
- `{"op":"screen_start"/"screen_stop","agent":"...","resolution":"1920x1080"}`

Worker → server:
- `{"op":"hello","worker":"<name>","caps":{"gpu":true,"arch":"x86_64"}}` — first frame, auth'd
- `{"op":"exec_chunk","id":"<job>","stream":"stdout","data":"..."}` — streamed
- `{"op":"exec_done","id":"<job>","exit_code":0,"duration_ms":123}`
- `{"op":"status","agent":"...","running":true}` — heartbeat payload

Every exec the worker performs is reported; the SERVER writes the exec-log
row (single audit trail, unchanged schema, plus a `worker` column).

## Security

- Enrollment: admin runs `hive-server enroll-worker <name>` → one-time
  token. Worker redeems it on first connect for a permanent worker key
  (`hmwk_...`, hashed in AuthStore like API keys). TLS from day one —
  this is the forcing function for the deferred Caddy/TLS decision:
  worker traffic crosses real networks, unlike today's LAN.
- Secrets: the server never hands the worker the secrets DB. At exec
  time the server resolves the agent's env (as today) and includes the
  *rendered* env-file CONTENT in the exec frame. It crosses TLS once per
  exec and lives only in the worker's tmpfs for that exec. The worker
  cannot enumerate or store secrets.
- The worker validates every frame against a strict op allowlist; it
  never executes a raw shell string from the wire (argv arrays only).

## Placement

Agents gain `worker` (default `"local"` = the server's own podman).
ContainerManager becomes a dispatcher: local agents take today's path;
remote agents route through the worker channel. `/api/agents/:id/container/*`
and exec-log/screen routes look up the agent's worker first.

Screen for remote agents: the worker's screen ports are NOT reachable by
the server. The worker reverse-proxies its noVNC/websockify over the same
WS channel (binary frames). Same URLs, same cookies — the client can't
tell the difference.

Workspace: v1 gives remote agents their own per-agent workspace on the
worker (no shared company share across machines — cross-machine sync is
its own project; note in docs if attempted).

## Failure behaviour

- Worker disconnects → its agents show `offline` in the roster (state
  tag infra exists); turns fail fast with "worker offline", do NOT
  silently fall back to local execution (that would leak the workload
  to the wrong machine).
- Server restarts → workers reconnect with backoff; in-flight execs are
  reported lost and the turn gets an error it can retry.
- Worker upgrades: hive-worker is versioned; server refuses workers whose
  protocol major differs (hello carries `proto:1`).

## Build order

1. **30.1** ✅ Worker registry server-side: enroll/revoke CLI, `workers`
   table, `agent.worker` column, GET/DELETE /api/workers.
2. **30.2** ✅ The channel: **dedicated WS port 8423** (`worker_ws_port`
   in server.json) — OmniNet's WebSocket::Server wraps its own HTTP
   server and cannot share the main API port; Caddy can proxy
   /api/workers/channel → 8423 later for the single-port TLS story.
   hello(key)→welcome, heartbeat→last_seen, `online` flag live.
   hive-worker binary: `--server ws://host:8423 --key hmwk_… --name N`,
   exponential-backoff reconnect. NOTE: the worker deliberately installs
   NO SIGINT/SIGTERM handler — default kill closes the socket so the
   server sees the disconnect immediately (a graceful flag would hang
   in the blocking receiveText).
3. ~~**30.3** exec dispatch over the channel (streamed chunks → exec-log)~~ — **DONE** (verified with real podman on dev-rig 2026-08-26).
   This alone makes remote agents useful.
4. ~~**30.4** Screen-over-WS reverse proxy~~ — **DONE** (verified with real
   podman on dev-rig 2026-08-26). Generic TCP streams over the channel
   (`stream_open`/`stream_data`(base64)/`stream_close`), synchronous
   `screen_ctl` (start/stop/status — worker runs the ContainerManager
   podman steps: browser image, `-p 127.0.0.1::6080`, hive-screen.sh),
   and a `ChannelSocket` (OmniNet::Socket facade over a stream) so the
   existing `/screen/ws` splice + asset proxy run unchanged. Verified:
   vnc.html + rfb.js through the tunnel, WS splice delivers the RFB
   greeting. NOTE: streams have no port allowlist — the channel is
   key-authenticated and `exec` already grants a full shell.
5. **30.5** TLS everywhere (Caddy on the server, worker pins the CA) —
   prerequisite for calling this production.

Non-goals for Phase 30: worker autoscaling, cross-machine shared
workspaces, worker-to-worker migration of running containers.


## OmniNet WebSocket pitfalls (learned 2026-08-26, do NOT rediscover)

1. **`Connection::receiveText()` drops coalesced frames** — its buffer is
   per-call; two frames in one TCP segment lose the second. Both sides of
   this channel use the `onText` callback API + `Connection::readLoop()`
   (which keeps its leftover across reads).
2. **`onClose` only fires on clean close frames.** Abrupt EOF (kill -9,
   network drop) silently breaks readLoop. The server therefore drives
   `ws.readLoop()` itself inside the handler and cleans up (unregister +
   fail in-flight jobs) after it returns.
3. **`keepAliveTimeout` (default 5s) becomes SO_RCVTIMEO, inherited by the
   upgraded WS socket** — 5s of worker silence read as peer-closed and
   killed execs mid-`podman create`. The channel sets
   `cfg.keepAliveTimeout = 0` (blocking reads).

## Phase 33 — worker-native skills, MCP tunnel, file push

Three more channel ops make the rig self-sufficient (no inbound ports, no
files left behind on the server):

- **Skill sync** (`skill_begin`/`skill_file`/`skill_end` → `skill_ack`,
  `skill_remove`): enabling a global skill for a worker agent streams the
  skill directory (base64 chunks, 50 MB cap) into
  `~/.local/share/hive-worker/skills/<name>/` and symlinks the agent's
  container home `.agents/skills/<name>` → `/skills/<name>` (the skills
  library is bind-mounted `/skills:ro`). The server re-pushes enabled
  skills on every worker reconnect (reconcile), so rig state survives
  reinstalls. Removal deletes both copy and link.
- **MCP tunnel** (`mcp_rpc` → `mcp_rpc_reply`): per-agent MCP servers are
  declared in the roster (`PATCH /api/agents/:id` `{"mcp_servers":
  [{"name","command","args":[],"env":{}}]}`). For worker agents, AgentHost
  seeds the agent's per-agent daedalus SettingsStore with a
  `streamable-http` entry pointing at the loopback **McpBridge**
  (`http://127.0.0.1:8424/mcp/<agent>/<server>`, `mcp_bridge_port` in
  server.json) and registers the real spawn spec with the bridge. Each
  JSON-RPC POST becomes an `mcp_rpc` op; the worker lazily spawns the
  server INSIDE the agent's container (`podman exec -i hive-<agent> …`),
  bridges newline-delimited stdio, serializes per (agent, server), and
  reaps processes idle >10 min. Local agents get `stdio` entries running
  `podman exec -i` directly from the server. Notifications (no JSON-RPC
  id) are fire-and-forget (HTTP 202).
  Debug: `GET /api/agents/:id/mcp-tools` shows the seeded config, what
  daedalus loaded, and which `mcp__<server>__<tool>` names are registered.
- **File push** (`file_push` → `file_push_ack`): `POST
  /api/agents/:id/push-file?path=<workspace-rel>` (raw body, 10 MB cap)
  streams a user-supplied input INTO the rig's shared workspace. The file
  lands on the rig only — it is never copied to the server (file
  locality: artifacts stay where produced; only requested files cross,
  via the existing `file_fetch`).

Verified live (2026-08-29): remotion-create skill synced to dev-rig and a
worker agent scaffolded + rendered a 3 s mp4 entirely on the rig; a
Blender 4.5 MCP server rendered a PNG through the tunnel from the agent's
normal tool loop; push-file round-tripped an md5-verified binary.

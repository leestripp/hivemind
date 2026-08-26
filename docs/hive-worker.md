# hive-worker — remote container execution (Phase 30 design)

Status: ACTIVE (un-deferred 2026-08-26). Build order 30.1→30.5 in
Plan.md §5. Tested loopback on dev-rig (worker + server same box,
ws://localhost) — TLS (30.5) only gates REAL remote workers.

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

1. **30.1** Worker registry server-side: enroll/revoke, `workers` table,
   `agent.worker` column, roster shows worker + online state.
2. **30.2** The channel: WS endpoint + hello/heartbeat + reconnect.
   Worker binary skeleton that connects and answers status.
3. **30.3** exec dispatch over the channel (streamed chunks → exec-log).
   This alone makes remote agents useful.
4. **30.4** Screen-over-WS reverse proxy.
5. **30.5** TLS everywhere (Caddy on the server, worker pins the CA) —
   prerequisite for calling this production.

Non-goals for Phase 30: worker autoscaling, cross-machine shared
workspaces, worker-to-worker migration of running containers.

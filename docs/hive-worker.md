# hive-worker (v2) — fleet manager + comms router

A hive-worker runs on any Linux machine (including the hive-server host
itself, for always-online agents) and owns everything compute: podman
containers, the agents inside them, and the files they produce.

## Connection

One outbound WebSocket to the hub (`wss://<server>/ws/worker`), auth'd
with a worker key issued at enrollment (`POST /api/workers/enroll` on the
hub). No inbound ports. Reconnects with backoff; on reconnect the hub
re-syncs the skills library and each agent's desired state, so a rig can
be wiped and rebuilt from the hub.

## Fleet management

The worker owns a container per agent (`hive-<agent-name>`):

- Image: `hive-agent` image (ubuntu base + hive-agent binary + common
  runtimes). Built locally from `hive-worker/Containerfile` or pulled
  from a registry the hub advertises.
- Mounts: shared `/workspace` (all agents on this worker collaborate),
  per-agent home volume (history/memory/daedalus.db), `/skills` read-only
  (synced library at daedalus' global skills location).
- User namespace: `--userns=keep-id` so container writes land as the
  worker user on the host.
- Liveness: heartbeat reports per-agent state; a crashed hive-agent is
  restarted automatically (its home volume makes restarts seamless).

## Task routing

```
hub ──task──▶ worker ──stdin──▶ hive-agent (in container)
hub ◀─events─ worker ◀─stdout── hive-agent
```

The worker speaks NDJSON to hive-agent over the container's stdio
(`podman exec -i` session or PID-1 pipe). Events: `delta` (streaming
text), `message` (final reply + attachment metadata), `approval_request`,
`image` (show_image intent), `status` (agent state changes).

## Files

- `file_fetch` / `file_push` ops move bytes between hub and the worker's
  workspace on explicit request only (file locality).
- Agent artifacts stay on the worker until someone asks for them.

## MCP

MCP servers are spawned by hive-agent itself, inside the container, as
ordinary child processes (stdio) or direct network clients. The worker's
only MCP job is making sure the container image/env has what the declared
servers need. No tunnels.

## Skills

The worker receives the skills library from the hub (`skill_sync` ops:
begin/file/end/remove), stores it under
`~/.local/share/hive-worker/skills/`, and bind-mounts it read-only into
containers at daedalus' global skills dir. Per-agent enable lists come
from the hub at spawn time (mount subset or env filter).

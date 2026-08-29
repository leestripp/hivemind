# Containers (v2) — the agent's home

Every HiveMind agent lives in a podman container on its worker. The
container is not a sandbox we reach into — it is where the agent (the
daedalus brain, packaged as **hive-agent**) actually runs.

## Layout

| Mount | Source | Mode | Purpose |
|-------|--------|------|---------|
| `/workspace` | worker's shared workspace dir | rw | Shared by all agents on the worker — this is how teammates collaborate on files |
| `$HOME` | per-agent host dir (home volume) | rw | daedalus history, memory, config — survives container replacement |
| `/skills` | worker's synced skills library | ro | Mounted at daedalus' global skills location; every agent sees the same skills |

Rootless podman with `--userns=keep-id`: files created in-container land
on the host owned by the worker user.

## The payload: hive-agent

The container's main process is hive-agent, a thin daedalus host:

- Reads its identity + model credentials from env (injected by the worker
  at spawn; credentials live on the worker host only).
- Speaks NDJSON on stdio: tasks in, events out (delta / message /
  approval / image / status).
- Skills load from `/skills` automatically (daedalus scans its global
  skills dir); skill helper scripts execute locally — same machine as the
  brain, no sync tricks.
- MCP servers are local child processes (stdio) or direct network
  clients. Full MCP support, in-container, no shortcuts.

## Lifecycle

The worker creates containers on agent spawn (`agent_spawn` op), starts/
stops them on demand, restarts crashed agents, and reaps containers when
an agent is deleted from the roster. The hub never talks to podman.

## Networking

Containers share the worker host's network namespace by default (simple,
matches v1 behavior). Agents that need isolation can be moved to a slirp
netns later — the agent doesn't know the difference either way.

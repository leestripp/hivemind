# Containers (v2) — the agent's home

Every HiveMind agent lives in a podman container on its worker. The
container is not a sandbox we reach into — it is where the agent (the
daedalus brain, packaged as **hive-agent**) actually runs.

## Layout

| Mount | Source | Mode | Purpose |
|-------|--------|------|---------|
| `/workspace` | worker's shared workspace dir | rw | **The agent's project root.** All agents on the worker share it — and daedalus derives everything from it |
| `$HOME` | per-agent host dir (home volume) | rw | Non-daedalus dotfiles (browser profiles, app config) — survives container replacement |
| `/skills` | worker's synced skills library | ro | Mounted at daedalus' global skills location; every agent sees the same skills |

Running at project root `/workspace` puts the daedalus state tree on the
shared volume:

```
/workspace/
├── .daedalus/                    # one subfolder per agent on this worker
│   ├── daedalus-Sticky/          #   history, memory, daedalus.db
│   └── daedalus-Bob/
├── .agents/skills/               # project-local skills — auto-scanned
│                                 #   by EVERY agent (shared, zero mounts)
└── … the actual work …
```

This is deliberate: agent state, shared skills and work product live in
ONE inspectable tree that the worker can back up or migrate wholesale.

Rootless podman with `--userns=keep-id`: files created in-container land
on the host owned by the worker user.

## The payload: hive-agent

The container's main process is hive-agent, a thin daedalus host:

- Starts with working dir = `/workspace` (its daedalus project root) —
  state lands in `/workspace/.daedalus/daedalus-<Name>/`.
- Reads its identity + model credentials from env (injected by the worker
  at spawn; credentials live on the worker host only).
- Speaks NDJSON on stdio: `task` ops in, events out (ready / delta /
  message / approval_request / send_agent_message / status / log).
  Full mapping table: `docs/hive-worker.md`.
- Skills load from `/skills` (global mount) AND `/workspace/.agents/
  skills/` (project-local, shared) automatically; helper scripts execute
  locally — same machine as the brain, no sync tricks.
- MCP servers are local child processes (stdio) or direct network
  clients. Full MCP support, in-container, no shortcuts.

## Lifecycle

The worker creates containers on agent spawn (`agent_spawn` op), starts/
stops them on demand, restarts crashed agents, and reaps containers when
an agent is deleted from the roster. The hub never talks to podman.

## Networking

Containers on a worker share ONE rootless network namespace managed by
podman + pasta (DNS forwarder 169.254.1.1). It is created when the first
container starts.

OPERATIONAL GOTCHA (hit 2026-08-30): if the pasta process dies, ALL
container networking dies with it -- agents stay "running" but every LLM
call fails with daedalus "Network error" retries while `getent hosts` in
the container returns nothing. Diagnosis: `pgrep pasta` as the worker
user. Recovery: `podman stop` ALL containers on the worker, then start
them again -- restarting just one while others still run REATTACHES it
to the dead netns instead of recreating it.

Since Phase 38 the worker self-heals single-container DNS wedges: its
60s tick probes `getent hosts github.com` (timeout-wrapped) in every
hive-* container; after 3 consecutive failures it `podman restart`s the
container when the agent is idle (busy agents log a journal warning and
are recycled once idle). The full-netns procedure above is only needed
when EVERY container fails at once.

## On-demand desktop (Phase 36)

The desktop stack (Xvfb + x11vnc + websockify/noVNC + Chromium under
openbox) is NOT always running. Every container publishes loopback 6080
at create time, so `hive-screen.sh start` (via `screen_ctl`) is a pure
`podman exec` — the agent brain survives. `hive-screen.sh stop` kills
the x11vnc watchdog first (pidfile) then the stack. The worker's idle
watcher counts ESTABLISHED :5900 connections and stops the screen after
HIVEMIND_SCREEN_IDLE_MIN (default 15) viewerless minutes — measured
reclaim ~350–700 MB per idle agent.

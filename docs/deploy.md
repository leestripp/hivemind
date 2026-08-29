# Deployment (v2)

HiveMind deploys as a hub plus any number of workers. The smallest real
setup is ONE machine running both: hive-server (hub) + hive-worker
(always-online agents).

## Topology

```
droplet (or any always-on box)          rigs (workstations, GPU boxes)
┌──────────────────────────┐            ┌───────────────────────────┐
│ hive-server  :8421 HTTPS │◀───WS──────│ hive-worker (per machine) │
│ hive-worker  (optional)  │            │  └─ podman containers     │
│  └─ containers           │            │     └─ hive-agent         │
└──────────────────────────┘            └───────────────────────────┘
```

## hive-server (hub)

- systemd --user service `hive-server`, binary + `web/dist` static dir.
- Data under `~/.local/share/hivemind/`: threads.db, roster, workers,
  skills library, workspace (attachment store).
- Config `server.json`: port, worker channel port, auth keys.
- TLS: terminate at the reverse proxy in front (the client speaks HTTPS
  to the proxy; the hub itself is plain HTTP on localhost).
- v2: no daedalus, no LLM keys, no podman on the hub host (unless it
  also runs a worker).

## hive-worker

- systemd --user service `hive-worker-<hostname>`; config `worker.json`
  with the hub URL + worker key from enrollment.
- Needs: rootless podman, subuid/subgid ranges, the hive-agent container
  image (built from `hive-worker/Containerfile` — the hub can hand out a
  registry reference instead).
- Data under `~/.local/share/hive-worker/`: workspace, skills library,
  per-agent home volumes, model/provider credentials (local LLM store —
  credentials never live on the hub).
- The worker dials OUT to the hub; rigs behind NAT need nothing else.

## Co-hosted worker (always-online agents)

Run hive-worker on the hub's own machine. Agents placed on worker
"droplet" are online whenever the hub is. This replaces v1's "local"
agents.

## Migrating from v1

Threads/messages/roster DBs carry over (the hub keeps them). Agents get
their `worker` field set (existing "local" agents → the co-hosted
worker). Agent history moves: v1 kept daedalus history on the hub under
`agents/<id>/`; the cutover tool copies each agent's home dir into its
container home volume on the target worker.

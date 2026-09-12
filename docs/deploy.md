# Deployment (v2)

> Operational runbook (per-rig commands, verification gates, pitfalls):
> the `deploy-hivement` agent skill (`.agents/skills/deploy-hivement/SKILL.md`).
> This doc is the topology/reference overview; keep both in sync.

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
- v2: no hive-brain, no LLM keys, no podman on the hub host (unless it
  also runs a worker).

## hive-worker

- systemd --user service `hive-worker`; config `worker.json`
  with the hub URL + worker key from enrollment.
- Needs: rootless podman, subuid/subgid ranges, the hive-agent container
  image chain — `:latest` → `:browser` → `:gpu`, built from
  `hive-server/data/Containerfile.hive-agent{,-browser,-gpu}`, ALL with
  repo-root build context. The chain copies the host-built hive-agent
  binary, so build it before the images.
- Data under `~/.local/share/hive-worker/`: workspace, skills library,
  per-agent home volumes, model/provider credentials (local LLM store —
  credentials never live on the hub).
- The worker dials OUT to the hub; rigs behind NAT need nothing else.
  Preferred URL is TLS through the hub's reverse proxy:
  `--server wss://<server>` (nginx proxies `/api/workers/channel` →
  8423 with Upgrade headers and 24h timeouts). A worker co-hosted with
  the hub uses `ws://127.0.0.1:8423` — loopback plaintext is fine.
  With all remote workers on wss, port 8423/tcp can be closed in the
  firewall (`sudo ufw delete allow 8423/tcp`).

## Co-hosted worker (always-online agents)

Run hive-worker on the hub's own machine. Agents placed on worker
"droplet" are online whenever the hub is. This replaces v1's "local"
agents.

## Development environment

The dev agent runs ON THE HOST system (moved out of the nested sandbox,
2026-08-29): podman works natively there, so hive-agent/hive-worker
container testing happens locally — build, run, and beat on containers
directly. The droplet stays a deployment target, not a test proxy.

## Migrating from v1

Threads/messages/roster DBs carry over (the hub keeps them). Agents get
their `worker` field set (existing "local" agents → the co-hosted
worker). Agent history moves: v1 kept hive-brain history on the hub under
`agents/<id>/`; the cutover tool copies each agent's home dir into its
container home volume on the target worker.

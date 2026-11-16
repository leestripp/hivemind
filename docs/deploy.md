## Host TCP grant rollout / rollback

The per-agent `host_tcp_ports` feature needs the new hub AND worker binaries;
the GTK/web controls alone are not a rollout. The hub schema addition is
additive (`agents.host_tcp_ports TEXT NOT NULL DEFAULT '[]'`) and existing
rows remain empty/default-deny. Use the disposable proof in
[containers.md](containers.md#disposable-integration-proof) on each Podman rig
before deployment; it must not be retargeted at production agents.

No image rebuild is required for a grant-only edit. Worker network-policy
drift recreates just that agent's container, preserving its current image and
pins. Rolling back to an older worker fails closed: it ignores the new pin and
uses default pasta on any subsequent recreation, revoking host-loopback TCP
access rather than retaining an unrecognized policy. Restore the paired
hub/worker version to manage grants again.

# Deployment

> This doc is the topology/reference overview. Operational runbooks
> (per-rig commands, verification gates) are site-specific — keep yours
> with your own deployment tooling, not in this repo.

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
- The hub runs no hive-brain and needs no podman unless it also runs a
  worker. It does hold provider/model configuration and provider API keys
  in `llm.db`; include that file in the hub's storage/backup threat model.

## hive-worker

- systemd --user service `hive-worker`; config `worker.json`
  with the hub URL + worker key from enrollment.
- Needs: rootless podman, subuid/subgid ranges, the hive-agent container
  image chain — `:latest` → `:browser` → `:gpu`, built from
  `hive-server/data/Containerfile.hive-agent{,-browser,-gpu}`, ALL with
  repo-root build context. The chain copies the host-built hive-agent
  binary, so build it before the images.
- Data under `~/.local/share/hive-worker/`: workspace, skills library,
  and per-agent home volumes. Current task model/provider configuration
  comes from the hub in each task frame; workers relay it without using a
  worker-local registry as the dispatch source of truth.
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

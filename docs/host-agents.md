# Host Agents (Phase 72)

A **host agent** runs **bare-metal on the worker machine** as a real unix
user (`hivex-<name>`) with **passwordless sudo** — no podman container, no
sandbox, no noVNC screen. It exists for agents that must administer the
machine itself: installing packages, managing services, touching files
outside any container.

## ⚠ READ THIS BEFORE ENABLING

A host agent can do **anything root can do**. Through error,
hallucination, or prompt injection it can **destroy the machine**. Only
run host agents on machines you don't mind losing. Container agents
remain the default and the recommendation for everything else.

The safety model is *informed, repeated consent*, not technical
containment:

1. **Worker opt-in** — the worker must be started with
   `--allow-host-agents` (or `HIVE_ALLOW_HOST_AGENTS=1`). Default is OFF;
   a hub frame alone can never conjure one. Opted-in workers advertise
   `host_agents: true` in their hello capabilities.
2. **Manual local provisioning** — only `sudo hive-worker
   provision-host-agent <name>` on the rig creates the unix user, the
   sudoers entry and installs the binary. Nothing reachable via the hub
   API can mint a sudo user. The command prints a red warning and
   requires typing the machine's hostname.
3. **API ack gate** — creating (or converting) a host agent requires the
   literal string `I UNDERSTAND` in `host_agent_ack`; without it the hub
   answers **400**. On a worker without the capability it answers **409**.
4. **Approvals forced on** — a host agent is created with
   `require_approval=true`. Turning approvals OFF on a host agent needs
   the same `I UNDERSTAND` ack.
5. **Journal audit** — every creation/enable/disable writes a
   `HOST AGENT …` line to the hub log.
6. **Unmissable UI** — the GTK client and web UI show a red **HOST ⚠**
   badge and the red warning block wherever a host agent appears or is
   configured.

## Provisioning (on the worker rig)

```sh
sudo hive-worker provision-host-agent rooty
# prints the red warning; type the machine's hostname to proceed
```

This creates:

| Piece | Where |
|-------|-------|
| unix user | `hivex-rooty` (home `/home/hivex-rooty`, password **locked**) |
| sudoers | `/etc/sudoers.d/hivex-rooty` — `NOPASSWD: ALL` (validated with `visudo -c` before install) |
| worker launch rights | the hive-worker account may `sudo -u hivex-rooty` run exactly: `tee`/`chmod` of `/home/hivex-rooty/.hive-env` and `/usr/local/bin/hive-agent` |
| binary | `/usr/local/bin/hive-agent` (shared by all host agents on the rig) |

Then create the agent in the UI (worker dropdown → enable "Host agent" →
type `I UNDERSTAND`) or via the API.

Removing one:

```sh
sudo hive-worker deprovision-host-agent rooty
# stops its processes, archives the home to
# /var/backups/hivex-rooty-<timestamp>.tar.gz, removes user + sudoers
```

## Runtime mechanics

- The worker launches the agent as
  `sudo -n -H -D /home/hivex-<name> -u hivex-<name> /usr/local/bin/hive-agent
  /home/hivex-<name> --env-file /home/hivex-<name>/.hive-env`.
- Secrets ride the **0600 env file** (staged via the worker's sudoers
  `tee`/`chmod` grant), never argv — `ps` is world-readable.
- `hive-agent --env-file` applies `KEY=VALUE` lines before config load;
  it also accepts the sandbox/workspace/personality vars the container
  path injects, so the agent binary is identical in both worlds.
- No container means: no noVNC screen, no container image, no
  restart-on-config-change via podman — the respawn loop treats it as a
  plain process. Host agents skip `ensure_container` entirely.
- `memory`/`cpus` caps are podman-only and do not apply to host agents.

## API

`POST /api/agents` and `PATCH /api/agents/:id` accept:

| Field | Type | Notes |
|-------|------|-------|
| `host_agent` | bool | `false` needs no ack; `true` requires the ack |
| `host_agent_ack` | string | must be exactly `I UNDERSTAND` when enabling host mode or disabling approvals on a host agent |

Errors: **400** missing/incorrect ack (response body carries the
warning); **409** target worker lacks the `host_agents` capability.
Roster JSON exposes `host_agent` on every agent.

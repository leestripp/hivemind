# Host Agents (Phase 72)

A **host agent** runs **bare-metal on the worker machine** as a real unix
user (`hivex-<name>`) with **passwordless sudo** — no podman container, no
sandbox, no noVNC screen. It exists for agents that must administer the
machine itself: installing packages, managing services, touching files
outside any container.

## The model: containers and host agents are the same shape

A host agent is NOT a new concept — it is the container model with real
unix parts substituted for podman parts:

| | Container agent | Host agent |
|---|---|---|
| **Per-agent state** | `home-<id>` dir → bind-mounted `/home/hive` | real unix home `/home/hivex-<name>` (sudo `-H`) |
| **Shared workspace** | worker workspace → bind-mounted `/workspace` | the host's real `/workspace` mount — cwd for every host agent on the rig |
| **On delete** | container + `home-<id>` removed; workspace untouched | unix user + home removed (home archived to `/var/backups/`); `/workspace` untouched |

Same shape, same cleanup boundaries: per-agent state dies with the
agent; the shared company drive is permanent. If you understand what a
container agent gets, you already understand what a host agent gets.

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
2. **Rig-level root opt-in** — `sudo hive-worker
   install-host-agent-support --worker-user <account>` on the rig
   installs two root-owned helper scripts (`hivex-provision`,
   `hivex-deprovision`) plus a scoped sudoers grant covering exactly
   those scripts. Nothing reachable via the hub API can mint a sudo
   user on a rig that never ran this — the helpers + grant ARE the
   consent. After it, creating a host agent in a client auto-provisions
   the unix user at spawn; deleting one auto-deprovisions.
3. **API ack gate** — creating (or converting) a host agent requires the
   literal string `I UNDERSTAND` in `host_agent_ack`; without it the hub
   answers **400**. On a worker without the capability it answers **409**.
4. **Approvals forced on** — a host agent is created with
   `require_approval=true`. Turning approvals OFF on a host agent needs
   the same `I UNDERSTAND` ack.
5. **Journal audit** — every creation/enable/disable writes a
   `HOST AGENT …` line to the hub log.
6. **Unmissable UI** — host agents are a first-class **Type** choice in
   the clients' agent dialog ("Bare-Metal ⚠", offered only when the
   selected worker advertises the capability).
   Selecting it reveals the red warning block + `I UNDERSTAND` entry at
   the bottom, forces approvals on, and hides the container-only rows
   (memory/CPU caps). Existing host agents show a red **HOST ⚠** badge
   wherever they appear.

## Setup (on the worker rig)

```sh
# ONCE per rig, as root — helpers + scoped sudoers grant:
sudo hive-worker install-host-agent-support --worker-user <account>
```

After that, **creating a host agent in a client is the whole flow**:
Type → Bare-Metal ⚠ → type `I UNDERSTAND` → save. At spawn the worker
auto-provisions via the `hivex-provision` helper, creating:

| Piece | Where |
|-------|-------|
| unix user | `hivex-<name>` (home `/home/hivex-<name>`, password **locked**) |
| sudoers | `/etc/sudoers.d/hivex-<name>` — `NOPASSWD: ALL` (validated with `visudo -c` before install) |
| worker launch rights | the hive-worker account may `sudo -u hivex-<name>` run exactly: `tee`/`chmod` of `/home/hivex-<name>/.hive-env` and `/usr/local/bin/hive-agent` |
| binary | `/usr/local/bin/hive-agent` (shared by all host agents on the rig; source: `--host-agent-binary` / `HIVE_HOST_AGENT_BINARY`, default auto-resolved next to the worker executable) |

Manual per-agent provisioning remains for repair/audits:

```sh
sudo hive-worker provision-host-agent rooty
# prints the red warning; type the machine's hostname to proceed
```

Removing one:

```sh
sudo hive-worker deprovision-host-agent rooty
# stops its processes, archives the home to
# /var/backups/hivex-rooty-<timestamp>.tar.gz, removes user + sudoers
```

**Deleting the agent from the roster cleans everything up.** `DELETE
/api/agents/:id` stops the process, drops the roster row, and the worker
auto-deprovisions: processes killed, home archived to
`/var/backups/hivex-<name>-<timestamp>.tar.gz`, unix user and sudoers
entry removed. The hub logs a `HOST AGENT deleted …` audit line. If the
worker was offline at delete time the cleanup can't be delivered — the
audit line says so; run `deprovision-host-agent` on the rig to finish.

**The workspace is SHARED and NEVER cleaned up.** With the worker's
`--host-workspace-root DIR` (env `HIVE_HOST_WORKSPACE_ROOT`) every host
agent on the rig runs with cwd = `DIR` itself — the same shared company
drive the containers see bind-mounted at `/workspace` (on ubuntu-pc that
IS the host's `/workspace` RAID mount). Provision's `--workspace-root
DIR` just verifies the mount exists; nothing per-agent is created, and
neither roster delete nor deprovision ever touches the workspace.
Without the flag the agent's cwd is its home dir (which IS
archived+removed on delete).

## Runtime mechanics

- The worker launches the agent as
  `sudo -n -H -u hivex-<name> /usr/local/bin/hive-agent
  /home/hivex-<name> --env-file /home/hivex-<name>/.hive-env`.
  No `-D`/`--chdir`: sudo-rs (Ubuntu 25.x default) rejects it even with a
  sudoers `CWD=` tag — hive-agent chdirs into the project root itself,
  matching the container path's `podman -w /workspace` exactly.
- Secrets ride the **0600 env file** (staged via the worker's sudoers
  `tee`/`chmod` grant), never argv — `ps` is world-readable.
- `hive-agent --env-file` applies `KEY=VALUE` lines before config load;
  it also accepts the sandbox/workspace/personality vars the container
  path injects, so the agent binary is identical in both worlds.
- No container means: no noVNC screen, no container image, no
  restart-on-config-change via podman — the respawn loop treats it as a
  plain process. Host agents skip `ensure_container` entirely.
- `memory`/`cpus` caps are podman-only and do not apply to host agents.

## Runtime descriptor env (2026-10-12)

One hive-agent binary serves both worlds; the worker's env file says
which one it landed in (`runtime_env_lines` in fleet.h renders these):

| var | container | host agent |
|-----|-----------|------------|
| `HIVEMIND_RUNTIME` | `container` | `host` |
| `HIVEMIND_WORKSPACE_ROOT` | `/workspace` | the shared `--host-workspace-root` path |
| `HIVEMIND_SKILLS_DIR` | `/skills` (ro mount) | the worker's skill library by host path |
| `HIVEMIND_SERVICE_*` | podman `-e` (8000+ → per-agent host block) | env file, **identity mapping** — the agent binds the advertised port directly (`CONTAINER_BASE == PORT_BASE`), same `service_ports.txt` registry so blocks never collide on the rig |

Worker file ops resolve against the **agent's** workspace root
(`Fleet::workspace_root`, "" → shared staging workspace):

- `share_file` reads the SOURCE from the agent's workspace; the staged
  blob still lands in the shared staging `shared/<share_id>/` so every
  later fetch is runtime-agnostic.
- Chat attachments: the worker's promote step reads
  `attachments/<tid>/outgoing/` from the agent's workspace (this fixed
  QueenBee's silent attach_file loss — the tool reported success while
  the hub never received the file). The canonical copy stays in
  staging; `attachments/` also joined the provisioning symlink mapping
  (incoming/shared) so user→agent attachments land where the agent
  reads them.
- `file_fetch`/`file_list` accept an `agent` field (hub sends it);
  agent workspace first, staging fallback.
- Hub-driven `exec` for a host agent runs via the scoped sudo grant
  (`timeout`+`env` wrapped), NEVER `ensure_container`.
- Host spawn fires `<ws>/.hivemind/boot.sh` detached, same contract as
  the container boot hook.
- `POST /api/agents/:id/screen/start` returns **409** for host agents
  (no container to project a screen out of).

The agent's system prompt states the runtime explicitly (host agents
kept prescribing container remedies for host-side gaps — QueenBee
audit 2026-10-12).

## Browser tools on host agents (2026-10-13)

The container browser stack is baked into the image; a host agent has no
image, so the four layers land on the HOST instead (first done by
QueenBee on ubuntu-pc, self-provisioned with her NOPASSWD sudo):

1. **Driver**: `/usr/local/bin/browser_driver.py` (canonical copy from
   `hive-server/data/`; root-owned). The agent binary hardcodes this
   path — no driver file = every `browser_*` call errors.
2. **Screen helper**: `/usr/local/bin/hive-screen.sh` — HOST EDITION:
   Xvfb :99 only (no Xvnc/websockify; hub screen/Computer view stays
   409 for host agents by design). Auto-invoked by the driver when the
   X99 socket is missing; self-heals cold starts, no boot hook needed.
3. **Playwright + chromium**: system-wide, mirroring the Containerfile —
   `pip install --break-system-packages playwright`, then
   `PLAYWRIGHT_BROWSERS_PATH=/opt/ms-playwright playwright install
   chromium --with-deps` (root for /opt + apt deps; on a GNOME desktop
   host the libs are already present). The driver globs
   /opt/ms-playwright first, then `~/.cache/ms-playwright`, then
   google-chrome/chromium on PATH.
4. **Display isolation**: the agent's chromium runs on Xvfb :99 owned by
   the hivex user — it NEVER touches the user's GNOME session unless
   explicitly pointed there (below). CDP is host-loopback :9222;
   container browsers are isolated by their own netns, so no clash.

**Login handoff onto the user's desktop** (the noVNC "Computer view"
equivalent for host agents): set `HIVE_BROWSER_DISPLAY=:0` (or the
session's display) in the agent's env file via the worker and respawn.
The user must first grant X access from their session:
`xhost +SI:localuser:hivex-<name>`. Unset the knob to go back to
invisible Xvfb. The driver never auto-starts Xvfb over a non-:99 target.

**Hub admin exec on host agents**: `POST /api/agents/:id/container/exec`
routes through `/usr/local/sbin/hivex-exec` (root-owned, timeout-bounded)
— the ONE command the worker's sudoers grant whitelists per hivex user
(Phase 72.1; the Phase 72 timeout(1)/env(1) wrap matched no grant and
always failed). Refresh helpers after upgrade:
`sudo <worker-binary> install-host-agent-support --worker-user <user>`,
then re-run `hivex-provision` for each host agent.

## API

`POST /api/agents` and `PATCH /api/agents/:id` accept:

| Field | Type | Notes |
|-------|------|-------|
| `host_agent` | bool | `false` needs no ack; `true` requires the ack |
| `host_agent_ack` | string | must be exactly `I UNDERSTAND` when enabling host mode or disabling approvals on a host agent |

Errors: **400** missing/incorrect ack (response body carries the
warning); **409** target worker lacks the `host_agents` capability.
Roster JSON exposes `host_agent` on every agent.

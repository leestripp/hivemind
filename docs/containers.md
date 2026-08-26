# Containers — the agent's computer

> HiveMind agents never run commands on the host. Every containerized agent
> works inside its own rootless podman container as user `hive` with
> passwordless sudo.

---

## Setup (Ubuntu 26.04)

```bash
sudo apt install -y podman
# That's it for a single-user box — rootless podman works out of the box.
# Optional (keep containers alive after logout):
loginctl enable-linger $USER
```

The base image builds itself on first use (first agent turn or
`POST …/container/start`) — ~2–4 minutes once, layer-cached after:

```bash
# manual build, if you prefer:
podman build -t localhost/hive-agent:latest \
    -f hive-server/data/Containerfile.hive-agent hive-server/data
```

## What an agent gets

| Inside the container | Host source |
|---|---|
| `/workspace` | **Shared company folder** — the `workspace_root` setting, bind-mounted into every agent container. Default `~/.local/share/hivemind/workspace`; set your own (e.g. `~/projects`) in the web UI **Settings → Company workspace** or `PUT /api/config` (admin key). Applies to containers created after the change. |

### Workspace layout (2026-08-25)

The company drive follows a fixed skeleton (seeded on install; canonical
copy in `hive-server/data/workspace/`). `index.md` at the root is the map —
the agent system prompt tells every containerized agent to read it before
doing file work, so conventions live in ONE file instead of per-agent
config:

```
index.md                  ← the map + house rules (kebab-case, ISO dates,
                            README.md per project, archive don't delete…)
departments/
  engineering/            README.md per department (scope, agents,
  marketing/              conventions)
  executive/
  chief-of-staff/
projects/<name>/          cross-department initiatives (README.md each)
inbox/                    user drops requests/files here for agents
attachments/<thread-id>/  chat uploads (server-managed)
scratch/                  volatile temp work
archive/                  finished work — moved, never deleted
```

Agents work in their department folder (or a project folder for cross-dept
work) and keep README.md files current so any agent or human can pick work
up cold. Add a department by creating `departments/<name>/` + README.md and
adding one line to `index.md` — no agent/template changes needed.
| `/home/hive` | **Private** per-agent bind mount from `~/.local/share/hivemind/agents/<id>/home` |
| user `hive` + `sudo` (no password) | base image |
| git, curl, python3+pip, build-essential, jq, … | base image |
| limits: 4 GiB RAM, 2 CPUs | `podman create` flags |

Agents install whatever else they need mid-task (`sudo apt install …`,
`pip install …`); installed packages persist with the container.

## How commands get there

For containerized agents the host-side `execute_bash` is disabled and a
host-registered tool **`container_exec`** takes over:

```
agent tool call → ContainerBashTool → podman exec -u hive -w /workspace \
    hive-<agent_id> bash -lc '<command>'
```

### …or to another machine (Phase 30)

When the agent's `worker` field names an enrolled worker, the same call is
routed by **WorkerRouter** over the worker channel to that machine's
`hive-worker`, which runs the identical podman steps there (container
create/warm-up, exec with `HIVE_SECRET_*` env injection, chunked output
streaming back into the same exec-log). The base image builds on first use
**on the worker**. NOTE: worker agents get a per-agent workspace
(`workspace-<name>`) local to the worker machine — NOT the shared company
drive, and the `index.md` skeleton is NOT seeded there yet (candidate
follow-up: sync the skeleton + optionally shared-drive content over the
channel). See `docs/hive-worker.md`.

## Managing containers

```bash
curl localhost:8421/api/agents/<id>/container/status
curl -X POST localhost:8421/api/agents/<id>/container/start   # idempotent
curl -X POST localhost:8421/api/agents/<id>/container/stop
curl -X POST localhost:8421/api/agents/<id>/container/exec \
     -H 'Content-Type: application/json' -d '{"command":"ls -la /workspace"}'
```

Or directly: `podman ps --filter name=hive-`.

## Workspace modes

| `workspace_mode` | Meaning |
|---|---|
| `shared` | Containerized; shared `/workspace` + private home |

**Containers are mandatory** (decision 2026-08-25). The former `host`
escape hatch (no container, no sudo, no isolation) is REMOVED — legacy
`host` rows in agents.db normalize to `shared` on load. The web API
`workspace_mode` field remains for forward compatibility but only
`shared` is accepted/used.

Department volumes (named volumes mounted into a chosen subset of agents)
are a planned extension of the shared mode.

## Browser/computer-use variant (Phase 6)

`hive-server/data/Containerfile.hive-agent-browser` layers the browser +
desktop stack onto the base image for agents that operate websites with no
API — including the **login handoff** flow (agent pauses at a login wall,
you authenticate via the live session view, control returns; the profile
persists in `/home/hive`).

> The display stack: Xvfb + openbox + x11vnc + noVNC/websockify +
> playwright chromium (Phase 25, working — verified with a real noVNC
> client). See docs/computer-use.md for the architecture, the wire path,
> operating instructions, and the failed Wayland detour (Phase 24).

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `container_exec` returns "podman is not installed" | `apt install podman` on the server machine |
| `newuidmap: write to uid_map failed` | running inside a nested/restricted container — run hive-server on a normal host/VM |
| first turn takes minutes | base image is building (once per machine) |
| agent can't see a teammate's file | it's in their private home; use `/workspace` for shared artifacts |
| exec fails with "worker … offline/unavailable" | the agent's assigned worker is disconnected — check the web Workers page, restart hive-worker there, or PATCH the agent back to `"local"` |


## Browser tools (Phase 6)

Containerized agents get `browser_*` tools alongside `container_exec`:

| Tool | Action |
|---|---|
| `browser_navigate` | open URL (returns final URL + title) |
| `browser_click` / `browser_type` / `browser_press` | interact by CSS selector / key |
| `browser_read` | page visible text (~6000 chars cap) |
| `browser_screenshot` | viewport PNG into the workspace |
| `browser_eval` | run JavaScript, JSON result |
| `browser_tabs` / `browser_back` / `browser_reload` | session navigation |
| `browser_fill_secret` | type a granted secret by NAME into a field |

Implementation: `hive-server/data/browser_driver.py` is copied into the
agent's home mount on each call and runs via `container_exec`. It connects
to a persistent Chromium over CDP (auto-launched on the Xvfb display :99),
so the browser the agent drives is the same one the user sees in the web
UI's Computer view — login handoff = user takes over, authenticates,
agent resumes on the same page. Profile persists at `~/.browser-profile`
in the agent's home mount.

`browser_fill_secret` checks the SecretStore grant on the host BEFORE any
container call; the value rides in via exec's `HIVE_SECRET_<NAME>` env
injection and the driver never echoes it — the secret never enters the
LLM context, tool args, or tool output.

Tools honor `require_approval` with the same approval queue as
`container_exec` (approval summaries contain the action + target, never
secret values).

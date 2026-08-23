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

| Inside the container | Source |
|---|---|
| `/workspace` | **Shared** named volume `hive-workspace` — every agent sees it ("the company") |
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
| `shared` (default) | Containerized; shared `/workspace` + private home |
| `host` | No container (escape hatch for dev machines without podman) |

Department volumes (named volumes mounted into a chosen subset of agents)
are a planned extension of the shared mode.

## Browser/computer-use variant (Phase 6)

`hive-server/data/Containerfile.hive-agent-browser` layers chromium,
playwright, Xvfb and noVNC onto the base image for agents that operate
websites with no API — including the **login handoff** flow (agent pauses
at a login wall, you authenticate via the live session view, control
returns; the profile persists in `/home/hive`). See the file header for the
build command and flow.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `container_exec` returns "podman is not installed" | `apt install podman` on the server machine |
| `newuidmap: write to uid_map failed` | running inside a nested/restricted container — run hive-server on a normal host/VM |
| first turn takes minutes | base image is building (once per machine) |
| agent can't see a teammate's file | it's in their private home; use `/workspace` for shared artifacts |

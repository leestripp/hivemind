# ⚡ HiveMind 2.0

![HiveMind](resources/banner.png)

A **self-hosted team of always-on AI agents** for Linux — spread across as
many machines as you own. Create named teammates, message them like
colleagues, and hand off real work — they run in their own podman
containers with sudo, remember everything, share files with each other,
and only come back when they need your approval.

![MIT license](https://img.shields.io/badge/license-MIT-blue)
![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)
![Platform](https://img.shields.io/badge/platform-Linux%20(Ubuntu%2026.04)-orange)

---

## What it does

- **Persistent named agents** — each teammate is a long-lived
  [daedalus](https://gitlab.com/leestripp/daedalus) agent with its own memory
  and conversation history that survives restarts.
- **Chat, not config** — talk to agents in threads (DMs or group threads);
  no workflow builder required. (The 1.x node-graph engine was retired in
  Phase 12 — threads + fan-in are the orchestration model.)
- **A computer of their own** — agents work inside rootless **podman
  containers** as user `hive` with passwordless sudo: they `apt install`
  whatever CLI tools the job needs. Shared company `/workspace` volume +
  private `/home/hive` per agent.
- **Teamwork** — agents discover, message and delegate to each other
  (`list_agents` / `message_agent` / `create_agent` tools), and deliver
  files to each other across machines with `send_file` — an agent on a
  remote rig drops finished work straight into the company workspace.
  A chief-of-staff agent can run the whole team.
- **Approvals** — flip `require_approval` on an agent and its commands park
  in an approval queue until you approve/deny them.
- **Distributed workers** (`hive-worker`) — enroll any Linux machine and
  assign agents to it: their containers, exec and screen sessions run on
  that machine over a persistent, auto-reconnecting WebSocket channel.
  Enroll with `hive-server enroll-worker <name>` (key shown once), run
  `hive-worker --server ws://<server>:8423 --key hmwk_… --name <name>`
  on the remote host, then set the agent's `worker` field. Live status on
  the web **Workers** page. Agents on the same rig share one `/workspace`
  (per-rig company drive), so they can collaborate on the same project
  files; `send_file` moves files between rigs and the server.
- **Live screen share** — every agent's container runs a full desktop
  (Xvfb + x11vnc + noVNC + Chromium); watch or take over from the client's
  **Computer** button — works for local *and* worker-hosted agents
  (tunneled through the worker channel).
- **Routines & skills** — scheduled prompts ("watch this, ping me daily")
  fired by a 30 s scheduler tick, and a **global skill library**
  (Anthropic SKILL.md format) shared across agents via per-agent enable
  toggles — plus per-agent learned skills that stay agent-owned.
- **Native GTK4 client** (`hive-client`) — roster with avatars, threads,
  live chat, approvals inbox, scheduled-task dialog, right-click agent
  management. Plus the **web dashboard**: Agents, Tasks, Files, Skills,
  Secrets, Models, Workers, API keys, Settings.
- **Any model** — providers/models live in HiveMind's own store
  (`llm.db`, managed on the web Models page) and are injected into agents
  at spawn; pick per agent. No vendor lock-in.

## Quick start (Ubuntu 26.04)

```bash
# 1. System packages
sudo apt install -y cmake g++ nlohmann-json3-dev \
    sqlite3 libsqlite3-dev libssl-dev libpng-dev libpoppler-cpp-dev \
    libgtkmm-4.0-dev podman   # no libcurl — OmniNet is the only HTTP stack

# 2. Build & run
./hivemindctl build
./hivemindctl start          # server on :8421
./hivemindctl client         # native GTK client
# or the web UI: xdg-open http://localhost:8421/
```

CMake fetches [daedalus](https://gitlab.com/leestripp/daedalus) (agent
engine) and [OmniNet](https://gitlab.com/leestripp/omninet) (HTTP server)
automatically.

### Models

Providers and models live in HiveMind's own store
(`~/.local/share/hivemind/llm.db`) — manage them on the web **Models** page
(`http://localhost:8421/#/models`) or via `/api/providers` + `/api/models`.
On first run with an empty store, existing entries are imported one-way
from the daedalus SettingsStore. Pass the model name when creating an agent
(blank = store default); credentials resolve at spawn time — daedalus never
reads the DB and is never patched.

## Layout

```
hive-server/   C++23 + OmniNet server: roster, threads, approvals,
               routines, containers, worker channel, web UI (web/dist)
hive-client/   GTK4/gtkmm4 native client
hive-worker/   Remote executor daemon — runs agent containers on other
               machines, driven over a WebSocket channel (Phase 30)
tests/         ctest suite
hivemindctl    build/start/stop/status/logs/client/test
```

## Security

- Bearer API keys (SHA-256 hashed at rest; master key shown once at first run)
- TLS via nginx + Let's Encrypt in front; server binds localhost-only
- Brute-force: fail2ban bans after 5 failed keys + nginx rate limiting
  (verified live — see docs/deploy.md §4b)
- Workers authenticate over `wss://` with per-worker keys; TLS verified
  (OmniNet client enforces cert verification against the system CA)
- Agent sandboxes: rootless podman containers; workspace writes are path-confined
  (send_file destinations can't escape the company drive)

## API sketch

```
POST/GET/PATCH/DELETE /api/agents[/:id]
POST/GET/DELETE       /api/threads[/:id]      GET/POST …/messages
GET  /api/threads/:id/activity                live turn state
GET  /api/approvals?status=pending            POST …/:id/approve|deny
POST/GET/DELETE       /api/routines[/:id]     POST …/:id/run
GET/POST/DELETE       /api/agents/:id/skills[/name]          local (agent-owned) skills
GET/POST/DELETE       /api/skills[/name]                     global skill library
GET/POST/DELETE       /api/secrets[/name]                    secret broker (write-only values)
GET/POST              /api/agents/:id/global-skills          per-agent global-skill toggle
GET/POST              /api/agents/:id/container/{status,start,stop,exec}
GET/DELETE            /api/workers[/:name]                   worker registry
GET/POST              /api/agents/:id/screen/{status,start,stop,password}
GET                   /screen/:agentid/…  + /screen/:agentid/ws   noVNC proxy
GET/POST/DELETE       /api/providers[/:name]  GET …/:name/discover
GET/POST/DELETE       /api/models[/:name]     POST …/:name/default
```

## Documentation

| Doc | Contents |
|---|---|
| `docs/api.md` | REST API reference (auth, roster, threads, routines, skills, containers, models) |
| `docs/deploy.md` | Remote deployment — droplet/VPS, systemd, Caddy TLS, lockdown |
| `AGENTS.md` | Roster agent concepts — memory, avatars, cascade delete, tools |
| `docs/skills.md` | Global skill library design — symlinks, local/global split |
| `docs/client.md` | GTK4 native client — dialogs, menus, theming |
| `docs/web.md` | Web dashboard — pages, stack, adding a page |
| `docs/containers.md` | Podman sandboxing |
| `docs/hive-worker.md` | Distributed workers — enroll, channel protocol, exec/screen ops |
| `docs/computer-use.md` | Live screen share (Xvfb/noVNC) design |
| `docs/design.md` | Architecture deep-dive |
| `Plan.md` | Roadmap & phase status |

See `docs/api.md` and `Plan.md` (roadmap, phases 0–30) for details.

Current release: **0.5.0** — distributed workers milestone.

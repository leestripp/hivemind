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
  [hive-brain](https://gitlab.com/leestripp/hive-brain) agent with its own memory
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
- **Agents send files to chat** — the `attach_file` tool copies any
  workspace artifact into the thread's attachment store and it lands as
  a normal chat attachment (images/videos render inline).
- **Approvals** — flip `require_approval` on an agent and its commands park
  in an approval queue until you approve/deny them.
- **Distributed workers** (`hive-worker`) — enroll any Linux machine and
  assign agents to it: their containers, exec and screen sessions run on
  that machine over a persistent, auto-reconnecting WebSocket channel.
  Enroll with `hive-server enroll-worker <name>` (key shown once), run
  `hive-worker --server wss://<server> --key hmwk_… --name <name>`
  on the remote host (TLS via the nginx proxy; `ws://<server>:8423`
  direct also works on a trusted LAN), then set the agent's `worker`
  field. Live status on
  the web **Workers** page. Agents on the same rig share one `/workspace`
  (per-rig company drive), so they can collaborate on the same project
  files; `send_file` moves files between rigs and the server.
- **On-demand desktops** — click the client's **Computer** button and the
  agent's container spins up a full desktop (Xvfb + x11vnc + noVNC +
  Chromium) in seconds; walk away and it auto-sleeps after
  `HIVEMIND_SCREEN_IDLE_MIN` (default 15) viewer-less minutes, reclaiming
  ~350–700 MB of RAM per idle agent. Works for local *and* worker-hosted
  agents (tunneled through the worker channel).
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

CMake fetches [hive-brain](https://gitlab.com/leestripp/hive-brain) (agent
engine) and [OmniNet](https://gitlab.com/leestripp/omninet) (HTTP server)
automatically.

### Models

LLM config is **worker-owned** in v2: each hive-worker keeps its own
`~/.local/share/hive-worker/models.json` (catalog) + `secrets.json`
(credentials, 0600) and injects them into agents as spawn env. The hub
holds no keys; `GET /api/models` returns the union of connected workers'
catalogs, which is what the client's New Agent dialog offers. Blank model
= the worker's default.

## Layout

```
hive-server/   C++23 + OmniNet server: roster, threads, approvals,
               routines, containers, worker channel, web UI (web/dist)
hive-client/   GTK4/gtkmm4 native client
hive-worker/   Remote executor daemon — runs agent containers on other
               machines, driven over a WebSocket channel (Phase 30)
*/tests/       per-app ctest suites
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
POST                  /api/threads/:id/attachments      user/agent file attachments
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
GET                   /api/files/download?path=[&worker=]           attachment/file download
GET                   /api/models           union of connected workers' catalogs
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

See `docs/api.md` and `Plan.md` (roadmap, phases 0–38) for details.

Current release line: **0.6.x** — on-demand desktops, chat attachments,
GNOME-adaptive client.

# ⚡ HiveMind 2.0

A **self-hosted team of always-on AI agents** for Linux. Create named
teammates, message them like colleagues, and hand off real work — they run
in their own podman containers with sudo, remember everything, coordinate
with each other, and only come back when they need your approval.

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
  (`list_agents` / `message_agent` / `create_agent` tools). A chief-of-staff
  agent can run the whole team.
- **Approvals** — flip `require_approval` on an agent and its commands park
  in an approval queue until you approve/deny them.
- **Routines & skills** — scheduled prompts ("watch this, ping me daily")
  fired by a 30 s scheduler tick, and a **global skill library**
  (Anthropic SKILL.md format) shared across agents via per-agent enable
  toggles — plus per-agent learned skills that stay agent-owned.
- **Native GTK4 client** (`hive-client`) — roster with avatars, threads,
  live chat, approvals inbox, scheduled-task dialog, right-click agent
  management. Plus the **web dashboard**: Agents, Tasks, Skills, Models,
  API keys, settings.
- **Any model** — providers/models live in HiveMind's own store
  (`llm.db`, managed on the web Models page) and are injected into agents
  at spawn; pick per agent. No vendor lock-in.

## Quick start (Ubuntu 26.04)

```bash
# 1. System packages
sudo apt install -y cmake g++ libcurl4-gnutls-dev nlohmann-json3-dev \
    sqlite3 libsqlite3-dev libssl-dev libpng-dev libpoppler-cpp-dev \
    libgtkmm-4.0-dev podman

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
               routines, containers, web UI (web/dist)
hive-client/   GTK4/gtkmm4 native client
tests/         ctest suite (9 tests)
hivemindctl    build/start/stop/status/logs/client/test
```

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
GET/POST/DELETE       /api/providers[/:name]  GET …/:name/discover
GET/POST/DELETE       /api/models[/:name]     POST …/:name/default
```

## Documentation

| Doc | Contents |
|---|---|
| `docs/api.md` | REST API reference (auth, roster, threads, routines, skills, containers, models) |
| `docs/agents.md` | Roster agent concepts — memory, avatars, cascade delete, tools |
| `docs/skills.md` | Global skill library design — symlinks, local/global split |
| `docs/client.md` | GTK4 native client — dialogs, menus, theming |
| `docs/web.md` | Web dashboard — pages, stack, adding a page |
| `docs/containers.md` | Podman sandboxing |
| `docs/design.md` | Architecture deep-dive |
| `Plan.md` | Roadmap & phase status |

See `docs/api.md` and `Plan.md` (roadmap, phases 0–8) for details.

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
  no workflow builder required. (The node-graph canvas still lives in the
  web UI for explicit orchestration.)
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
  and Anthropic-format agent skills per teammate.
- **Native GTK4 client** (`hive-client`) — roster, threads, live chat,
  approvals inbox. Plus the web UI as workflow studio + fallback.
- **Any model** — daedalus' SettingsStore owns providers/credentials; pick
  per agent. No vendor lock-in.

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

Agents need a model in the daedalus **SettingsStore**
(`~/.local/share/daedalus/daedalus.db`) — manage it with
[icarus](https://gitlab.com/leestripp/icarus). Pass the model name when
creating an agent (blank = store default).

## Layout

```
hive-server/   C++23 + OmniNet server: roster, threads, approvals,
               routines, containers, graph runner, web UI (web/dist)
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
GET/POST/DELETE       /api/agents/:id/skills[/name]
GET/POST              /api/agents/:id/container/{status,start,stop,exec}
```

See `docs/api.md` and `Plan.md` (roadmap, phases 0–8) for details.

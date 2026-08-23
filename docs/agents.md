# Roster Agents — the HiveMind 2.0 teammate model

> User-facing concepts: what a roster agent is, how memory and naming work,
> what agents can do, and how to run a team.

---

## What is a roster agent?

A roster agent is a **persistent, named AI teammate**. You don't write
prompts at it once — you *work* with it over time:

- It has a **name** and a **role** (persona), set at creation.
- It **remembers**: full conversation history and learned memory survive
  server restarts.
- It has **hands**: a podman container of its own where it runs commands
  as user `hive` with sudo (installs its own tools).
- It can **talk to the team**: discover, message and delegate to other
  agents, even create new ones.
- It can be **gated**: with `require_approval`, every command it wants to
  run waits for your approval first.

Create one via the GTK client (**+ Agent**) or the API:

```bash
curl -X POST localhost:8421/api/agents -H 'Content-Type: application/json' -d '{
  "name": "Nova",
  "role": "research assistant — concise answers",
  "model": "kimi-for-coding"
}'
```

## Naming & memory (how persistence works)

The roster name becomes the daedalus **agent name** (sanitized to
`[A-Za-z0-9_.-]`). Each agent gets a directory:

```
~/.local/share/hivemind/agents/<agent-id>/
├── .daedalus/daedalus-<Name>/   # messages.json (history), memory, logs
├── .agents/skills/              # the agent's skills (SKILL.md dirs)
└── home/                        # bind-mounted to /home/hive in its container
```

On server start, an agent's first turn reconstructs its daedalus runtime
and reloads prior history — it picks up where you left off.

> **Graceful stops matter.** History is written on `Agent::shutdown()`, which
> the server calls on SIGTERM (`hivemindctl stop`, systemd, Ctrl-C).
> `kill -9` can lose the in-flight turn.

## Models

`model` is any name in the daedalus SettingsStore
(`~/.local/share/daedalus/daedalus.db`, managed by icarus). Blank = store
default. Different teammates can use different models/providers.

## Tools every roster agent has

| Tool | What it does |
|---|---|
| `container_exec` | Run a bash command in its container (`/workspace`, user `hive`, sudo) |
| `list_agents` | See the team |
| `message_agent` | Task a teammate and get its reply |
| `create_agent` | Add a teammate to the roster |
| `run_icarus` | Hand a self-contained task to an icarus subprocess |
| daedalus built-ins | memory tools, skills, MCP servers, etc. (host shell `execute_bash` is disabled for containerized agents) |

## Chief-of-staff pattern

Create an agent whose role is coordination:

> *"You are the chief of staff. Discover teammates with list_agents,
> delegate with message_agent, create specialists with create_agent when a
> lane is missing, and report back concisely."*

Then message only the chief — it routes work to the team. Delegation nests
up to 4 levels deep (loop protection).

## Approvals

Set `require_approval: true` for agents touching anything sensitive. Their
commands park in the **Approvals** inbox (client header shows a count);
approve/deny there or via the API. Ten minutes without a decision expires
the request and the agent is told it was denied.

## Routines (always-on behaviour)

A routine = named prompt + schedule, attached to an agent, with its own
thread (`routine: <name>`). The 30-second scheduler fires due routines; the
agent's reply appears in the routine thread.

```bash
curl -X POST localhost:8421/api/routines -H 'Content-Type: application/json' -d '{
  "agent_id": "<id>", "name": "morning-brief",
  "prompt": "Check my feeds and give me a 3-bullet brief.",
  "schedule": {"type": "daily", "time": "08:30"}
}'
```

## Skills

Save a proven workflow as a reusable skill (Anthropic SKILL.md format):

```bash
curl -X POST localhost:8421/api/agents/<id>/skills \
  -H 'Content-Type: application/json' -d '{
    "name": "weekly-report",
    "description": "Produce the Monday metrics report",
    "content": "# Weekly report\n\n1. Pull metrics …\n2. …"
  }'
```

"Teach" v1: narrate the steps once, save them as a skill, and the agent
reuses them — no re-explaining.

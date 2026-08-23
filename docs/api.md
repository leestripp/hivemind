# HiveMind 2.0 — REST API Reference

> Base URL: `http://127.0.0.1:8421` (default). All bodies are JSON.
> The server serves the web UI at `/` and the OmniNet admin dashboard at
> `/omninet/*` on the same port. Routes are first-match-wins.
>
> **Path params:** examples use `:id` segments literally; substitute real ids.

## Authentication (Phase 9)

Every `/api/*` route except `/api/health` requires a bearer key:

```
Authorization: Bearer hm_...
```

The **master key** is printed once at first server start (also in
`hivemindctl logs`). Create/revoke more via `/api/keys` (admin scope).
Scopes: `admin` (everything), `agent` (default), `read` (GET only).
Responses: `401` missing/invalid/revoked key, `403` insufficient scope.

| Method | Path | Description |
|---|---|---|
| GET | `/api/keys` | List keys (public ids only) — admin |
| POST | `/api/keys` | `{name, scopes}` → returns raw key **once** — admin |
| DELETE | `/api/keys/:id` | Revoke by public id — admin |

## Contents

1. [Health & meta](#1-health--meta)
2. [Roster (agents)](#2-roster-agents)
3. [Threads & chat](#3-threads--chat)
4. [Approvals](#4-approvals)
5. [Routines](#5-routines)
6. [Skills](#6-skills)
7. [Containers](#7-containers)
8. [Models](#8-models)
9. [Projects & files](#9-projects--files)

---

## 1. Health & meta

| Method | Path | Description |
|---|---|---|
| GET | `/api/health` | `{"status":"ok","service":"hivemind"}` |
| GET | `/api/agent/status` | daedalus agent-engine status |
| GET | `/api/agent/tools` | Built-in + MCP tools available to roster agents |
| GET | `/api/agent/skills` | Skills visible to roster agents |
| GET | `/api/agent/mcp` | MCP server status |
| GET | `/api/config` | Server config (`workspace_root` — the company share host folder) |
| PUT | `/api/config` | Update config `{workspace_root}` — **admin scope**; applies to containers created after the change |

## 2. Roster (agents)

A roster agent is a persistent, named teammate (see `docs/agents.md`).

| Method | Path | Description |
|---|---|---|
| POST | `/api/agents` | Create agent |
| GET | `/api/agents` | List roster |
| GET | `/api/agents/:id` | Get one agent |
| PATCH | `/api/agents/:id` | Update fields (partial) |
| DELETE | `/api/agents/:id` | Remove from roster (on-disk agent dir kept) |

**Create body**

```json
{
  "name": "Nova",                     // required, unique, sanitized to a daedalus agent name
  "role": "research assistant",       // persona injected into the system prompt
  "model": "kimi-for-coding",         // daedalus SettingsStore model; "" = store default
  "workspace_mode": "shared",         // "shared" = podman container (default), "host" = no container
  "require_approval": false           // true → commands park in the approval queue
}
```

**Agent object**

```json
{
  "id": "uuid", "name": "Nova", "role": "...", "model": "...",
  "workspace_mode": "shared", "container_id": "",
  "require_approval": false,
  "created_at": "ISO-UTC", "updated_at": "ISO-UTC"
}
```

## 3. Threads & chat

| Method | Path | Description |
|---|---|---|
| POST | `/api/threads` | Create thread → 201 thread object |
| GET | `/api/threads` | List threads (newest first, incl. participants) |
| GET | `/api/threads/:id` | Thread + `agent_ids` |
| DELETE | `/api/threads/:id` | Delete thread, messages and participants |
| GET | `/api/threads/:id/messages` | Messages, chronological |
| GET | `/api/threads/:id/messages?after=<msg_id>` | Incremental fetch (polling) |
| POST | `/api/threads/:id/messages` | Post user message → **202** `{message_id, status:"processing"}` |
| GET | `/api/threads/:id/activity` | Live turn state (see below) |

**Create thread body:** `{"title": "optional", "agent_ids": ["<id>", ...]}`
— more than one agent makes it a group thread.

**Post message body:** `{"body": "text"}`. Each agent participant starts a
background turn; replies appear as `sender_kind:"agent"` messages.

**Message object**

```json
{
  "id": "uuid", "thread_id": "uuid",
  "sender_kind": "user|agent|system", "sender_id": "agent-id-or-empty",
  "body": "…", "created_at": "ISO-UTCµs"
}
```

**Activity object** (`GET …/activity`)

```json
{
  "busy": true,
  "turns": [
    {"agent_id": "…", "agent_name": "Nova", "partial": "streamed text so far",
     "error": "", "done": false}
  ]
}
```

Clients poll `activity` (typing/streaming) and `messages?after=` (completed)
— this is exactly what hive-client does every 1.5 s.

## 4. Approvals

When an agent has `require_approval: true`, every command it wants to run
(host `execute_bash` or `container_exec`) creates a pending approval and the
turn **blocks** until resolved (10 min timeout → auto-`expired`).

| Method | Path | Description |
|---|---|---|
| GET | `/api/approvals` | List all approvals (newest first) |
| GET | `/api/approvals?status=pending` | Filter: `pending\|approved\|denied\|expired` |
| POST | `/api/approvals/:id/approve` | Approve (pending only) |
| POST | `/api/approvals/:id/deny` | Deny (pending only) |

**Approval object:** `{id, agent_id, agent_name, kind:"command", payload,
status, created_at, resolved_at}` — `payload` is the exact command.

## 5. Routines

Named, scheduled prompts attached to an agent. Each routine owns a thread
(`routine: <name>`); on fire, the prompt posts there and the agent replies
in-thread.

| Method | Path | Description |
|---|---|---|
| POST | `/api/routines` | Create → 201 |
| GET | `/api/routines` | List (incl. human `schedule_text`) |
| POST | `/api/routines/:id/run` | Fire immediately (off-schedule) → 202 |
| POST | `/api/routines/:id/enabled` | `{"enabled": true|false}` |
| DELETE | `/api/routines/:id` | Remove |

**Create body**

```json
{
  "agent_id": "uuid",
  "name": "heartbeat",
  "prompt": "Say hello and give the current time.",
  "schedule": {"type": "interval", "every_minutes": 30}
}
```

Schedule specs (server-local time):

| Type | Fields | Meaning |
|---|---|---|
| `once` | `at: "YYYY-MM-DDTHH:MM"` | Fire once, then disable |
| `interval` | `every_minutes: N` | Every N minutes |
| `daily` | `time: "HH:MM"` | Every day |
| `weekly` | `days: [0..6]`, `time: "HH:MM"` | Weekdays (0=Sun) |

## 6. Skills

Anthropic-format skills per agent, stored at
`~/.local/share/hivemind/agents/<id>/.agents/skills/<name>/SKILL.md`
(daedalus auto-discovers the project-relative `.agents/skills/` dir).

| Method | Path | Description |
|---|---|---|
| GET | `/api/agents/:id/skills` | List skill names |
| POST | `/api/agents/:id/skills` | Create/overwrite `{name, description, content}` |
| DELETE | `/api/agents/:id/skills/:name` | Remove |

`name` must be kebab-case (`a-z 0-9 -`, ≤64 chars). `content` is the
markdown body; frontmatter is generated.

## 7. Containers

| Method | Path | Description |
|---|---|---|
| GET | `/api/agents/:id/container/status` | `{exists, running, container, image, podman_available}` |
| POST | `/api/agents/:id/container/start` | Create + start (idempotent; builds image on first use) |
| POST | `/api/agents/:id/container/stop` | Stop (container kept) |
| POST | `/api/agents/:id/container/exec` | `{command, timeout?}` → `{exit_code, output, timed_out}` (admin/debug) |

Containers are named `hive-<agent_id>`; image `localhost/hive-agent:latest`.
See `docs/containers.md`.

## 8. Models

| Method | Path | Description |
|---|---|---|
### Providers & models (Phase 13)

HiveMind owns LLM configuration in its own LLMStore
(`~/.local/share/hivemind/llm.db`); first run imports from daedalus.db.
Credentials are resolved server-side and injected into each agent at
spawn — daedalus' SettingsStore is no longer consulted by HiveMind.

| Method | Path | Description |
|---|---|---|
| GET | `/api/providers` | List providers (keys never returned, `has_key` only) |
| POST | `/api/providers` | Create provider `{name, host, api_key?, timeout_seconds?}` |
| PATCH | `/api/providers/:name` | Update provider fields |
| DELETE | `/api/providers/:name` | Delete (400 while models reference it) |
| GET | `/api/providers/:name/discover` | Live `GET /v1/models` at the provider → `{models: [{id}]}` |
| GET | `/api/models` | List models (resolved host/has_key included) |
| POST | `/api/models` | Create `{name, provider, model_id, max_context_size?, temperature?, is_default?}` |
| GET | `/api/models/:name` | One model |
| PATCH | `/api/models/:name` | Update model fields |
| POST | `/api/models/:name/default` | Make the default model |
| DELETE | `/api/models/:name` | Delete model |

HiveMind never stores credentials — providers/keys live in the daedalus
SettingsStore (`~/.local/share/daedalus/daedalus.db`), managed by icarus.

## 9. Projects & files

| Method | Path | Description |
|---|---|---|
| GET | `/api/projects` | Known projects |
| GET | `/api/projects/:name` | Project detail |
| GET | `/api/fs/browse` | Filesystem browser for the web UI picker |

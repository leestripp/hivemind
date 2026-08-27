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

### Workers (Phase 30)

- `GET /api/system` — host facts + current sample (hostname, cores,
  uptime, mem/cpu/disk %, load, hive-server RSS, running containers)
- `GET /api/system/history?minutes=N` — 15 s samples for the last N
  minutes (max 24 h, in-memory ring buffer)
- `GET /api/system/agents` — per-agent usage rollup, last 24 h: exec
  count, timeouts, avg duration, last exec, messages sent
- `GET /api/workers` — list enrolled workers (name, caps, proto,
  last_seen, created_at, **`online`** — live from the worker channel :8423)
- `DELETE /api/workers/:name` — revoke (key dies with the row; connected
  worker drops and cannot reconnect)
- Enrollment is CLI-only on the server: `hive-server enroll-worker <name>`
  prints the raw `hmwk_…` key once (hash stored); `revoke-worker <name>`
  removes it. Agents gain `worker` (default `"local"`) via roster POST/PATCH.
- Agents whose `worker` names an enrolled worker execute **there**:
  container exec (30.3) and screen sessions (30.4) are routed over the
  channel transparently — the container/screen endpoints below are
  unchanged. See `docs/hive-worker.md` for the channel protocol.
Scopes: `admin` (everything), `agent` (default), `read` (GET only).
Responses: `401` missing/invalid/revoked key, `403` insufficient scope.

The server itself does NOT throttle failed auth — unlimited attempts get
401. On internet-facing installs, brute-force protection lives at the
nginx edge (fail2ban bans after 5×401, plus a 10 req/s rate limit) —
see docs/deploy.md §4b. A wrong key is never logged with its value.

**CORS** (Phase 9): default is no CORS headers — the web UI is same-origin.
For cross-origin browser clients, add exact origins (or `"*"`) to
`~/.local/share/hivemind/server.json` `"cors_origins"` and restart;
preflight `OPTIONS` is answered without auth. TLS terminates at Caddy —
see `docs/deploy.md`.

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
| GET | `/api/agents` | List roster (each carries `busy` = has a live turn) |
| GET | `/api/agents/:id` | Get one agent |
| PATCH | `/api/agents/:id` | Update fields (partial) |
| DELETE | `/api/agents/:id` | Remove from roster + **cascade** (see below). `?reassign_to=<id>` hands routines to another agent |

**Create body**

```json
{
  "name": "Nova",                     // required, unique, sanitized to a daedalus agent name
  "role": "research assistant",       // persona injected into the system prompt
  "model": "kimi-for-coding",         // LLMStore model name; "" = store default
  "worker": "local",                  // "local" or an enrolled worker name (Phase 30)
  "workspace_mode": "shared",         // "shared" = podman container (default), "host" = no container
  "require_approval": false,          // true → commands park in the approval queue
  "avatar": "engineering"             // dept avatar, e.g. "engineering", "sales-female"; "" = auto (name hash)
}
```

**Agent object**

```json
{
  "id": "uuid", "name": "Nova", "role": "...", "model": "...",
  "workspace_mode": "shared", "container_id": "",
  "require_approval": false, "avatar": "engineering",
  "created_at": "ISO-UTC", "updated_at": "ISO-UTC"
}
```

**DELETE cascade** — removing an agent cleans up what it owns:

- **Solo threads** (the agent is the only participant — its DM, routine
  threads) are deleted with their messages
- **Group threads** keep going without it: the agent is removed as a
  participant and a system note (`<name> was removed from the hive.`) posts
- **Routines** are deleted — or reassigned with `?reassign_to=<agent_id>`;
  reassigned routines swap their routine-thread participation to the new
  owner so they keep firing

Response reports what happened:

```json
{"deleted": true, "threads_deleted": 2, "threads_left": 1,
 "routines_deleted": 0, "routines_reassigned": 1}
```

> **Errors (Phase 19):** every error body is `{"error": msg, "request_id": "req-N-…"}`
> and every response carries `X-Request-Id`. The server logs `[req-…] error <status>: …`
> on stderr — quote the id when reporting issues.

## 3. Threads & chat

| Method | Path | Description |
|---|---|---|
| POST | `/api/threads` | Create thread → 201 thread object |
| GET | `/api/threads` | List threads by latest activity, incl. participants + `last_activity` (latest message ts — drives unread badges) |
| GET | `/api/threads/:id` | Thread + `agent_ids` |
| DELETE | `/api/threads/:id` | Delete thread, messages and participants |
| POST | `/api/secrets` | `{name, value}` stores encrypted; `{name, source}` (Phase 22) stores a vault reference — `pass:path` or `op://Vault/Item/field` — fetched from the vault CLI at delivery time, never persisted |
| POST/GET/DELETE | `/api/connectors[/:name]` | OAuth2 connector registry (Phase 23); writes admin-only, `client_secret` never listed |
| GET | `/api/connectors/:name/auth-url` | Mint one-shot state, return the provider authorize URL |
| GET | `/api/oauth/callback?code&state` | PUBLIC provider redirect target — state is the credential; exchanges code → tokens stored in the broker as `<NAME>_ACCESS_TOKEN` / `_REFRESH_TOKEN` |
| GET | `/api/skills/registry` | Curated marketplace catalog (Phase 21) — bundled `skills-registry.json` entries annotated with `installed` flags; install via existing `POST /api/skills/install` |
| GET | `/api/templates` | Teammate templates catalog (Phase 19) — `{templates:[{id, name, role, avatar, require_approval}]}`; pre-fill/one-click the normal agent create. 10 presets, one per department head (executive … operations), each with its dept avatar; every role tells the agent to read `/workspace/index.md` for its department folder (the index carries the mapping) |
| GET | `/api/threads/:id/messages` | Messages, chronological; each has an `attachments` array (`[]` default, Phase 18) |
| GET | `/api/threads/:id/messages?after=<msg_id>` | Incremental fetch (polling) |
| POST | `/api/threads/:id/messages` | Post user message → **202** `{message_id, status:"processing"}`. Optional `attachments` array (Phase 18) — metadata from prior attachment uploads |
| POST | `/api/threads/:id/attachments?name=<fn>` | Upload a file (raw body, ≤10 MB) → **201** `{name, rel_path, path, host_path, size}`. Saved under `<workspace>/attachments/:id/`; containers see it at `/workspace/…` |
| GET | `/api/files?path=<rel>` | List a workspace directory (dirs first) → `{path, entries:[{name, path, kind, size, mtime}]}`. Confined to the workspace root |
| GET | `/api/files/download?path=<rel>` | Download one workspace file (`Content-Disposition: attachment`) |
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
| DELETE | `/api/routines/:id` | Remove (also deletes the routine's own thread — routines own their threads) |

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

Two layers (see `docs/skills.md` for the design): a **global library**
managed by the server, and **local skills** owned by each agent.

### Global library — `~/.local/share/hivemind/skills/`

| Method | Path | Description |
|---|---|---|
| GET | `/api/skills` | Library list: `{name, description, version, enabled_agents[]}` |
| POST | `/api/skills` | Create `{name, description, content}` → 201 (409 if exists) |
| DELETE | `/api/skills/:name` | Remove from library **and unlink every agent** |
| POST | `/api/skills/install` | Install from git `{git_url, path?}` — shallow clone, validate SKILL.md, copy into library (409 on name conflict, 502 on clone failure) |
| GET | `/api/agents/:id/global-skills` | Enabled global skill names for the agent |
| POST | `/api/agents/:id/global-skills` | Toggle: `{name, enabled}` — creates/removes a relative symlink in the agent's `.agents/skills/` (409 if a local skill owns the name) |

### Local (per-agent) skills

Anthropic-format skills in
`~/.local/share/hivemind/agents/<id>/.agents/skills/<name>/SKILL.md`
(daedalus auto-discovers the project-relative `.agents/skills/` dir). These
belong to the agent — the teach-by-demonstration flow writes here; there is
no central management UI for them.

| Method | Path | Description |
|---|---|---|
| GET | `/api/agents/:id/skills` | List skill names |
| POST | `/api/agents/:id/skills` | Create/overwrite `{name, description, content}` |
| DELETE | `/api/agents/:id/skills/:name` | Remove |

`name` must be kebab-case (`a-z 0-9 -`, ≤64 chars). `content` is the
markdown body; frontmatter is generated.

## 6.5 Secrets (Phase 15)

The secret broker delivers credentials to agents **without** the values
entering the LLM context (context = history = logs = leak). Values are
AES-256-GCM-encrypted at rest (`~/.local/share/hivemind/secrets.db`,
master key `secrets.key` 0600, created on first use) and are
**write-only** over the API — no endpoint ever returns a value.

| Method | Path | Description |
|---|---|---|
| GET | `/api/secrets` | List `{name, created_at, agents[]}` (grants resolved) — **values never included** |
| POST | `/api/secrets` | `{name, value}` → 201 (409 if exists — rotation = delete + re-add) — admin |
| DELETE | `/api/secrets/:name` | Delete + cascade grants — admin |
| GET | `/api/agents/:id/secrets` | Granted secret names |
| POST | `/api/agents/:id/secrets` | Toggle grant: `{name, granted}` — admin |

`name` is `[A-Za-z0-9_]`, starts with a letter/underscore, ≤64 chars.
Multiline values are rejected (the delivery channel is line-based).

**Delivery:** `POST /api/agents/:id/container/exec` injects granted secrets
as `HIVE_SECRET_<NAME>` env vars via `podman exec --env-file` (0600 tmp
file, deleted when exec returns) — values never appear in podman's argv,
shell history, or tool output. The Phase 6 browser `fill_secret` tool will
type values via playwright for web logins.

## 6.4b Approval rules (Phase 16 — rule-based auto-review)

Per-agent ordered rules that short-circuit the human approval queue.
First match wins; no match falls through to the normal flow (queue when
`require_approval` is on). Patterns are shell-style globs (`*` = any run,
`?` = one char) on the tool kind (`command`, `browser`) and the payload.

| Method | Path | Description |
|---|---|---|
| GET | /api/agents/:id/approval-rules | `{rules: [{tool, command, action}]}` in evaluation order |
| PUT | /api/agents/:id/approval-rules | Replace the list atomically (≤100; action `allow`\|`deny`) |

Auto decisions are recorded as approvals rows with status
`auto_allowed`/`auto_denied` (resolved immediately — they never appear as
pending) so the inbox shows what the rules did.

## 6.6 Screen (Phase 6 — computer use)

All under the standard `/api/` Bearer auth; screen control requires a
non-read-only key (POST).

| Method | Path | Description |
|---|---|---|
| GET | /api/agents/:id/screen | Screen status: `{podman, browser_image, running, port, url}` |
| POST | /api/agents/:id/screen/start | Ensure browser container (`--shm-size 1g`, recreated on image-digest change) + display stack (Xvfb/openbox/x11vnc/websockify + Chrome). 502 with the podman error on failure. Returns `{running, port, url}`. |
| POST | /api/agents/:id/screen/stop | Stop the display stack (container keeps running) |

**Browser-facing proxy** (noVNC can't send Bearer headers — these live
outside `/api/` and authenticate via `?key=` query, which sets an HttpOnly
`hive_screen` cookie (Path=/screen, SameSite=Strict) for subsequent asset
and WebSocket requests; admin or agent scope required):

- `GET /screen/:id/` → 302 → `vnc.html?autoconnect=true&resize=scale&path=%2Fscreen%2Fws%3Fagent%3D<id>&password=<vnc-pw>`
  (path is percent-encoded ABSOLUTE — relative doubles against the page
  URL; password is the per-agent 8-char VNC password, so noVNC
  autoconnects with no prompt)
- `GET /screen/:id/<asset>` → HTTP proxy to the container's `127.0.0.1:6080`
- `WS  /screen/ws?agent=<id>` → WebSocket splice: hive-server completes the
  client handshake itself, then opens a fresh upgrade to the container's
  websockify and relays raw bytes both ways.

The container publishes 6080 on **127.0.0.1 only** — hive-server is the
single auth-guarded entry point, safe behind Caddy/TLS. Browser profile
persists in the agent's `/home/hive` bind mount, so login sessions survive
container restarts (login handoff: agent reports the wall in-thread, user
opens the Computer view, authenticates, hands back).

## 7. Containers

| Method | Path | Description |
|---|---|---|
| GET | `/api/agents/:id/container/status` | `{exists, running, container, image, podman_available}` |
| POST | `/api/agents/:id/container/start` | Create + start (idempotent; builds image on first use) |
| POST | `/api/agents/:id/container/stop` | Stop (container kept) |
| POST | `/api/agents/:id/container/exec` | `{command, timeout?}` → `{exit_code, output, timed_out}` (admin/debug) |

Containers are named `hive-<agent_id>`; image `localhost/hive-agent:latest`.
See `docs/containers.md`.

### Exec audit log (Phase 8)

| Method | Path | Description |
|---|---|---|
| GET | /api/agents/:id/exec-log?limit=100 | Every command the agent ran in its container (container_exec, browser_*, screen stack), newest first: `{entries: [{id, ts, command, exit_code, timed_out, duration_ms}]}`. Commands are verbatim (≤500 chars) — secret values never appear in argv (env-file injection), so the log doubles as broker accountability. |

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

# HiveMind design (2026-08-29)

HiveMind is a team of persistent AI agents that live in containers on
worker machines, with a central communications hub and a native GTK4
client. This document describes the current architecture.

## Principles

1. **The brain lives where the work is.** Agents (hive-brain) run inside
   their containers on workers — skills, MCP servers, shell access and
   files are all local to the agent. Nothing is tunneled.
2. **The server is a comms hub and control plane, not an agent host.**
   It routes messages, owns the provider/model registry, and stores
   provider credentials used to configure tasks. Agent workloads run on
   workers, not in the hub process.
3. **Outbound-only workers.** Workers dial the hub over one WebSocket;
   rigs need no inbound ports, no DNS, no TLS setup.
4. **File locality.** Work artifacts stay on the machine that produced
   them; bytes cross machines only when explicitly requested (file_fetch,
   show_image, push_file).
5. **Linux-only.** podman (rootless), systemd --user, GTK4/gtkmm4.

## Components

### hive-server — the comms hub

- Owns: threads, messages, attachments, roster, skills library
  (canonical copy), worker registry, and the Phase-43 provider/model
  registry (`llm.db`).
- Routes: client→agent tasks, agent→client replies, agent↔agent messages
  (pair threads, generation-capped).
- Does NOT: run agents, spawn containers, or proxy shells. Provider API
  keys do live in `llm.db`; protect the hub home and backups accordingly.

### hive-worker — fleet manager + router

- One per machine (including the server's own host for always-online
  agents).
- Spawns/owns podman containers; each container runs **hive-agent**.
- Desktops are on-demand: `screen_ctl start` execs the in-container
  `hive-screen.sh` (no container recreate — loopback 6080 is published
  at create time); a 60s idle watcher tears the stack down after
  `HIVEMIND_SCREEN_IDLE_MIN` (default 15) viewerless minutes, and
  health-checks container DNS (auto-restart after 3 wedged probes when
  the agent is idle).
- Shared `/workspace` mount per worker so its agents collaborate on the
  same files; per-agent home volumes for history/memory.
- Synced skills library mounted read-only into every container at
  hive-brain' global skills dir.
- Relays the task protocol between hub and the in-container agents.

### hive-agent — the brain (in-container)

- Thin host around `hive-brain::Agent` (icarus-style wiring), running with
  project root = the worker's shared `/workspace` — every agent's
  hive-agent state lives side by side at `/workspace/.hive-agent/<Name>/`, and
  `/workspace/.agents/skills/` is a shared project-local skills dir all
  agents auto-scan.
- stdio NDJSON task protocol in; delta/message/image events out.
- Full hive-brain feature set: skills with scripts, MCP servers spawned as
  local child processes, persistent memory + conversation history on the
  home volume.
- Provider/model configuration arrives in each task's `llm` block. The
  hub resolves it from `llm.db`; the worker relays it without persisting
  it, and hive-agent applies it before the turn. Environment-based model
  configuration is only a legacy/standalone fallback.

#### External-content and secret-disclosure boundary (B1)

`SystemPromptBuilder` gives every agent a static, non-optional **External
Content and Secrets** section, independent of its roster role, runtime, model,
or per-agent prompt override. Web pages, forums, social posts, documents, and
tool/MCP results are untrusted data rather than a source of instruction
authority. Embedded demands to override the prompt, run actions, or expose
credentials must not expand the authorized task. Secret values must not be
echoed into chat, posts, logs, URLs, or unauthorized destinations; legitimate
`secrets_get` and `browser_fill_secret` use remains available without
disclosure.

This is model-level hardening, not a replacement for container isolation,
scoped grants, host-agent warnings, or verification of external actions. The
section stays before volatile session memory to preserve the static prompt
prefix used by provider caches.

### hive-client — single voice-enabled GTK4/gtkmm4 app

- Roster, threads, chat, streaming replies, rich media, agent management.
- `hive-client/voice/` is linked into this same process as `VoiceView`:
  ChimpFlow STT/TTS, PipeWire audio, per-agent voices, message narration and
  a page-scoped XDG Talk shortcut. There is no standalone voice executable.
- Hub conversations use the hub REST API; local voice I/O uses ChimpFlow.
  The client knows nothing about containers or workers beyond presence and
  worker-routed attachment downloads.

## Comms flow at a glance

```
hive-client (chat + embedded voice) --REST :8421 (Bearer)--> hive-server
hive-client/voice --local HTTP :8090--> ChimpFlow (STT/TTS)
                                      |  threads, roster, routines,
                                      |  routines, files; no LLM keys
hive-worker --WS :8423 /api/workers/channel--> hive-server
   hello+hmwk_ key in; welcome back; then:
   hub->worker: task, cancel, agent_spawn/stop/remove,
                exec, file_*, skill_*, screen_ctl, stream_*
   worker->hub: heartbeat, agent_status, task_delta, task_result,
                agent_msg, agent_log, exec_*, file_*,
                skill_ack, screen_result, stream_*
hive-worker --podman exec -i (NDJSON stdio)--> hive-agent (in container)
   stdin:  task, cancel, shutdown
   stdout: ready, delta, message,
           send_agent_message, status, log
```

Full op tables and the load-bearing transport rules (readLoop only,
keepAliveTimeout=0, per-connection writer queues) are in
`docs/hive-worker.md`.

## Data flow: a user message

1. Client POSTs the message to the hub's thread API.
2. Hub stores it and looks up each agent participant's worker placement.
3. Hub resolves the agent's provider/model from `llm.db` and sends a
   `task` op, including the resolved `llm` configuration, down that
   worker's authenticated WebSocket.
4. Worker writes the task to the agent container's hive-agent stdin.
5. hive-agent runs the turn (skills/MCP/shell all local to the container),
   streaming `delta` events; the final `message` event carries the reply
   plus any attachment metadata (e.g. images to show). Files the agent
   explicitly attaches (`attach_file` tool →
   `/workspace/attachments/<thread>/outgoing/`) are swept up by the
   worker at turn end and arrive as attachment records on `task_result`
   (the hub injects the worker name so clients can route downloads).
## Hub transcripts versus agent brain context

The hub's `threads.db` is the durable, per-thread user-visible transcript
used by the voice-enabled GTK client, Android, and other REST clients. It is not the exact
prompt history sent to a model. Each hive-agent also maintains one
hive-brain conversation at `/workspace/.hive-agent/<Name>/messages.json`
on its worker. That conversation is shared across the agent's hub threads,
routines, and triggers, with source/timestamp labels added to wakes. A
per-thread hub view therefore does not imply per-thread model-context
isolation; use separate agents where that isolation is required.

## Durable agent messaging (Phase 66)

Messages persist in ThreadStore; the **delivery intents** (wakes that tell
an agent a message exists) are durable rows in `deliveries.db`
(DeliveryStore). Every wake/relay moves through
`queued → dispatched → done | error | dropped | expired`:

- **Busy receivers stay `queued`** — the hub dispatches one wake per idle
  transition (turn completion / agent-ready / watchdog), so a wake can
  never be tombstoned by the turn watchdog while queued agent-side.
- **Hub restarts requeue orphans** (`dispatched → queued` on boot) and
  draining resumes as agents report ready. Terminal rows prune after 7d.
- **Idempotency** per (target, source message) replaces content-window
  dedup on the wake path; content dedup remains only as double-POST
  protection for the message row itself.
- **Gates are checked at dispatch time and recorded** — a mute or a
  circuit-breaker trip marks the row `dropped` with the reason instead
  of vanishing silently.
- **Relays are durable and arm from any thread kind** (user DM, routine
  check-in, group); a fired relay suppresses the requester's pair-thread
  fan-in (single wake path — no more duplicate deliveries).
- **Read receipts**: per-(thread, agent) high-water marks; advanced when
  a wake completes (the message was the agent's prompt) and on own posts.

Agent-facing protocol (hub↔worker↔agent):
`message_agent` is a confirmed `msg_send_req`/`msg_send_resp` round-trip
returning the `delivery_id`; the `message_status` tool
(`msg_status_req`/`msg_status_resp`) lists an agent's recent sends or one
delivery, with live state and read receipts. Admin observability:
`GET /api/deliveries?agent=&state=&limit=`.

6. Worker relays events up the channel; hub stores the reply as a
   message; the client's poll picks it up and renders (images download
   through the hub's file endpoint).

## One-shot agent event triggers

`TriggerStore` (`triggers.db`) holds agent-owned event subscriptions. The
initial event is `agent_idle`: the watched agent has no live turn, **no
queued wake, and no active managed background process**. This is deliberately
stricter than a single `task_result`, so a trigger cannot fire between queued
turns or while the agent is waiting for `execute_bash` background work.
Creating a trigger also evaluates the target immediately, closing the
just-finished race.

`BackgroundProcessManager` reports its active count on start, natural exit,
explicit stop, cleanup, and initial agent startup. The worker forwards these
as `background_state`; the hub persists non-zero counts in `triggers.db` so a
hub restart cannot forget background work. A real agent restart reports its
fresh count, while worker reconciliation does not spuriously clear it.
`background_done` also carries the remaining count. Natural process exit still
posts its ordinary completion nudge: the idle trigger waits for that wake and
its resulting turn to finish rather than racing the process-exit event.

Agents use `create_trigger`, `list_triggers`, and `delete_trigger`. These ride
the authenticated `trigger_req`/`trigger_resp` worker bridge; there is no
public REST write surface. The hub resolves the target from the roster,
rejects self-watches, and enforces owner-only list/delete. Deleting an agent
removes both its own triggers and triggers watching it.

Each registration owns a solo `trigger` thread. On idle, the hub atomically
claims matching rows, posts a normal generation-0 user-style wake with
`source="trigger:agent_idle"`, then deletes the registration. The body names
the trigger, event, watched agent name/id, and `State: idle`, followed by any
creator instructions. The existing prompt edge renders
`[trigger:agent_idle @ <UTC ISO>]`; the stored body remains header-free.
Failed posting releases the claim, and opening the store recovers interrupted
claims. A process crash after posting but before completion can therefore
re-deliver after restart (at-least-once recovery rather than silent loss);
normal firing is one-shot. Firing preserves the thread's history; explicit
cancellation removes it.

## Chat fan-in storm gates (Phase 65)

Agent-to-agent fan-in is what makes threads collaborative — and what can
wedge the fleet when two agents politeness-loop. Containment lives
entirely hub-side (`ChatService`):

- **Mutes (65a)** — right-click Stop / `POST /api/threads/:id/cancel`
  mutes the affected agents (and thread) for ~10 min: generation>0
  fan-in tasks to muted targets are dropped and logged
  (`[chat] fan-in dropped (muted)`).
- **Circuit breaker (65b)** — >20 fan-ins in a 300 s window per thread
  trips the thread: further fan-ins dropped, one system message
  announced (`[chat] circuit breaker TRIPPED`).
- **Acks never fan (65c)** — `fanin_is_ack_only` (FanInPolicy.cpp)
  classifies pure acknowledgments; they POST to the thread but never
  wake the peer. This makes one-line closes free and is why the agent
  prompt (Phase 65j) tells directly-addressed agents to close visibly
  instead of staying silent.
- **Mention-gating (65h)** — in threads with >2 agents, an agent reply
  only fans to agents it explicitly mentions (roster-name match);
  2-agent threads keep broadcast.
- **Turn lifecycle (65g/65i/65j)** — agent-side, the set_status-only
  breaker ends turns whose only tool calls are `set_status` (the
  models' normal exit idiom). Output is NEVER wiped (65g reverted same
  day — it ate legitimate replies); clean Done/Idle exits log INFO.

Diagnostic shortcuts: thread turn state via
`GET /api/threads/:id/activity`; what a model ACTUALLY emitted in its
own `~/.local/share/hive-worker/workspace/.hive-agent/<Name>/messages.json`
on the worker host. Agents confabulate receipt of peer messages — trust
the DB (threads.db), not their claims.

# HiveMind design (v2, 2026-08-29)

HiveMind is a team of persistent AI agents that live in containers on
worker machines, with a central communications hub and a native GTK4
client. This document describes the v2 architecture — see Plan.md for the
migration phases.

## Principles

1. **The brain lives where the work is.** Agents (hive-brain) run inside
   their containers on workers — skills, MCP servers, shell access and
   files are all local to the agent. Nothing is tunneled.
2. **The server is a comms hub, not a host.** It routes messages between
   clients, workers and agents. It holds no LLM credentials and runs no
   workloads.
3. **Outbound-only workers.** Workers dial the hub over one WebSocket;
   rigs need no inbound ports, no DNS, no TLS setup.
4. **File locality.** Work artifacts stay on the machine that produced
   them; bytes cross machines only when explicitly requested (file_fetch,
   show_image, push_file).
5. **Linux-only.** podman (rootless), systemd --user, GTK4/gtkmm4.

## Components

### hive-server — the comms hub

- Owns: threads, messages, attachments, roster, approvals relay, skills
  library (canonical copy), worker registry.
- Routes: client→agent tasks, agent→client replies, agent↔agent messages
  (pair threads, generation-capped), approval round-trips.
- Does NOT: run agents, hold LLM keys, spawn containers, proxy shells.

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
  hive-brain state lives side by side at `/workspace/.daedalus/`, and
  `/workspace/.agents/skills/` is a shared project-local skills dir all
  agents auto-scan.
- stdio NDJSON task protocol in; delta/message/approval/image events out.
- Full hive-brain feature set: skills with scripts, MCP servers spawned as
  local child processes, persistent memory + conversation history on the
  home volume.
- Credentials arrive as spawn env from the worker — never stored on the
  hub.

### hive-client — GTK4/gtkmm4 (unchanged shape)

- Roster, threads, chat, streaming replies, inline images, approvals UI.
- Connects to the hub only; knows nothing about containers or workers
  beyond presence display.

## Comms flow at a glance

```
hive-client --REST :8421 (Bearer)--> hive-server
                                      |  threads, roster, approvals,
                                      |  routines, files; no LLM keys
hive-worker --WS :8423 /api/workers/channel--> hive-server
   hello+hmwk_ key in; welcome back; then:
   hub->worker: task, cancel, approval_resp, agent_spawn/stop/remove,
                exec, file_*, skill_*, screen_ctl, stream_*
   worker->hub: heartbeat, agent_status, task_delta, task_result,
                approval_req, agent_msg, agent_log, exec_*, file_*,
                skill_ack, screen_result, stream_*
hive-worker --podman exec -i (NDJSON stdio)--> hive-agent (in container)
   stdin:  task, approval_resp, cancel, shutdown
   stdout: ready, delta, message, approval_request,
           send_agent_message, status, log
```

Full op tables and the load-bearing transport rules (readLoop only,
keepAliveTimeout=0, per-connection writer queues) are in
`docs/hive-worker.md`.

## Data flow: a user message

1. Client POSTs the message to the hub's thread API.
2. Hub stores it and looks up each agent participant's worker placement.
3. Hub sends a `task` op down that worker's WebSocket.
4. Worker writes the task to the agent container's hive-agent stdin.
5. hive-agent runs the turn (skills/MCP/shell all local to the container),
   streaming `delta` events; the final `message` event carries the reply
   plus any attachment metadata (e.g. images to show). Files the agent
   explicitly attaches (`attach_file` tool →
   `/workspace/attachments/<thread>/outgoing/`) are swept up by the
   worker at turn end and arrive as attachment records on `task_result`
   (the hub injects the worker name so clients can route downloads).
6. Worker relays events up the channel; hub stores the reply as a
   message; the client's poll picks it up and renders (images download
   through the hub's file endpoint).

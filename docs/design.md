# HiveMind design (v2, 2026-08-29)

HiveMind is a team of persistent AI agents that live in containers on
worker machines, with a central communications hub and a native GTK4
client. This document describes the v2 architecture — see Plan.md for the
migration phases.

## Principles

1. **The brain lives where the work is.** Agents (daedalus) run inside
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
- Shared `/workspace` mount per worker so its agents collaborate on the
  same files; per-agent home volumes for history/memory.
- Synced skills library mounted read-only into every container at
  daedalus' global skills dir.
- Relays the task protocol between hub and the in-container agents.

### hive-agent — the brain (in-container)

- Thin host around `daedalus::Agent` (icarus-style wiring).
- stdio NDJSON task protocol in; delta/message/approval/image events out.
- Full daedalus feature set: skills with scripts, MCP servers spawned as
  local child processes, persistent memory + conversation history on the
  home volume.
- Credentials arrive as spawn env from the worker — never stored on the
  hub.

### hive-client — GTK4/gtkmm4 (unchanged shape)

- Roster, threads, chat, streaming replies, inline images, approvals UI.
- Connects to the hub only; knows nothing about containers or workers
  beyond presence display.

## Data flow: a user message

1. Client POSTs the message to the hub's thread API.
2. Hub stores it and looks up each agent participant's worker placement.
3. Hub sends a `task` op down that worker's WebSocket.
4. Worker writes the task to the agent container's hive-agent stdin.
5. hive-agent runs the turn (skills/MCP/shell all local to the container),
   streaming `delta` events; the final `message` event carries the reply
   plus any attachment metadata (e.g. images to show).
6. Worker relays events up the channel; hub stores the reply as a
   message; the client's poll picks it up and renders (images download
   through the hub's file endpoint).

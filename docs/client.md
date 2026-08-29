# hive-client — the GTK4 native client

> The chat-first daily driver for HiveMind 2.0. Linux (GTK4/gtkmm4),
> Ubuntu 26.04 target.

## Run

```bash
./hivemindctl client          # or: ./build/hive-client/hive-client
```

Connects to any hive-server — the **Connect** dialog (header button, or
automatic on auth failure) takes the server URL + API key and persists
them in `~/.config/hivemind/config.json`. Local dev default:
`http://127.0.0.1:8421` (start it with `./hivemindctl start`).

## Tour

- **Sidebar — Agents:** the roster (name + role). Double-click an agent to
  open a DM thread with it.
- **Sidebar — Threads:** every conversation, newest first. Routine threads
  appear as `routine: <name>`.
- **Chat:** message bubbles (you on the left, agents on the right), with a
  "X is working…" indicator while a turn streams. Completed replies append
  automatically (1.5 s poll).
- **Composer:** type, Enter to send. Every agent in the thread replies.
- **+ Agent:** name, role, **Provider/Model dropdowns** (live from the
  server's model store; model list filters by provider), workspace mode
  (`shared` = container, `host` = none), the require-approval switch, and an
  **avatar picker** — Auto (name hash) or one of the 20 department-head
  tiles.
- **+ Thread:** pick one or more participants (multi-select = group thread).
- **⏰ Add Task (header):** schedule a routine — name, agent, prompt, and a
  schedule type dropdown that swaps the fields below it (once: date+time /
  interval: minutes / daily: time / weekly: time + Su–Sa checks). Server
  validation errors show inline without closing the dialog. Also available
  per-agent via right-click → **Add Task…** (preselects the agent).
- **Right-click an agent** in the roster: Add Task… / Edit Agent (same
  dialog, prefilled) / Delete Agent. Delete shows a cascade summary — how
  many private threads will be deleted vs group threads that continue — and,
  when the agent owns routines, a "Delete them / Reassign to X" dropdown.
  Cancel is the default response.
- **Active thread:** the selected sidebar row is highlighted with an accent
  bar (re-applied after every refresh rebuild) and the chat header shows
  the thread's title — you can always tell which conversation you're in.
- **Approvals:** header button shows a live pending count; open it to
  approve/deny parked commands inline.

## Theme & icons

- **Settings dialog** (gear button in the header): two-pane layout —
  section sidebar left (Interface, User), settings stack right.
  - *Interface*: **dark/light mode** switch
    (`gtk-application-prefer-dark-theme`), **font scale** slider
    (75–175% via `gtk-xft-dpi`; 100% = follow the system DPI/scaling),
    read-only server address.
  - *User*: display name (replaces the "You" label on your bubbles) and
    avatar picker (default silhouette or any dept head); saving
    re-renders the open thread immediately.
  - All of it persists in `~/.config/hivemind/config.json`
    (`dark_mode`, `font_scale`, `display_name`, `user_avatar`).
- **Icons** (`data/icons/*.svg`) are 24×24 `currentColor`-stroked SVGs
  bundled into the binary via GResource (`glib-compile-resources`).
  GTK's pixbuf loader doesn't resolve `currentColor` from CSS, so
  `themed_icon_pixbuf()` string-replaces it with the theme foreground
  (`#eeeeec` dark / `#241f31` light) before rendering — that is what
  makes icons follow the theme.
- **Avatars** (`data/avatars/*.svg`) are a set of 20 cartoon
  department-head badges — 10 departments (executive, engineering,
  marketing, sales, hr, finance, it-support, design, legal, operations),
  each in male (`<dept>.svg`) and female (`<dept>-female.svg`) variants —
  plus `user.svg` for the human. They carry their own pastel/badge colors
  so they read on both themes. The New Agent dialog shows the full set as
  a picker (department pairs side by side, tooltips name each head);
  "Auto" (the default) stably hashes the agent's name into the set, and
  the roster's `avatar` field stores the chosen name.

## Right-click agent menu

**🖥 Computer** (Phase 26/30.4: the agent's live screen — POSTs
screen/start, then opens the noVNC view embedded in a WebKitGTK window;
falls back to the system browser when the client was built without
WebKitGTK 6.0). Works for worker-hosted agents too — the server tunnels
the screen through the worker channel. Note: the first open after a
container cold-start can race VNC startup — just click again. ·
Add Task… · **Skills…** (Phase 14.2: shared-library switches + read-only
learned-skills list) · **Inspector…** (Phase 14.4: agent details + container
status with Start/Stop) · Edit Agent · Delete Agent.

## Workers (Phase 30)

Agents assigned to a remote worker run their containers/screen there, but
the client experience is unchanged — chat, approvals, Computer, exec log
all work through the server regardless.

**Placement:** the New/Edit Agent dialog shows a **Worker** row whenever
at least one enrolled hive-worker is *online* (per `GET /api/workers`).
Options are "This server (local)" plus each online rig; offline rigs are
not offered (they can't start a container), and with none online the row
is hidden entirely. Editing an agent whose worker has since gone offline
still shows that rig flagged "(offline)" so the agent can be moved back
to the server. Worker status/enrolment lives on the web **Workers** page
and `hive-server enroll-worker <name>`.

## Attention indicators (Phase 14.1)

Unread ● on thread rows with activity newer than last-seen (clears on
select); ● on roster rows while the agent works (`busy` flag); desktop
notifications for new messages in unfocused threads and new parked
approvals (seeded on first refresh — no startup storm).

## Notes for contributors

- `ApiClient` calls are blocking by design — always invoked on detached
  worker threads, results marshalled to the GTK loop through a
  `Glib::Dispatcher` queue (`run_async`). Never block the main loop.
- No WebSocket yet: polling covers messages, activity and approvals.
- The client is pure UI: no containers, no credentials, no agent state.

## Chat images (Phase 34)

Image attachments (png/jpg/jpeg/gif/webp) render inline in the chat as a
scaled preview (max 480px, click to open full-size in the default viewer)
with the agent's caption below. Bytes are downloaded through the API on
first view and cached in
`~/.cache/hivemind-client/attachments/<thread_id>/` — re-opening a thread
renders from cache with no network traffic. Non-image attachments keep
the classic 📎 chip row. Agents attach images with their `show_image`
tool; user uploads with image extensions also render inline.

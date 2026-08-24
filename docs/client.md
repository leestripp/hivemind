# hive-client — the GTK4 native client

> The chat-first daily driver for HiveMind 2.0. Linux (GTK4/gtkmm4),
> Ubuntu 26.04 target.

## Run

```bash
./hivemindctl client          # or: ./build/hive-client/hive-client
```

Expects hive-server on `http://127.0.0.1:8421` (start it with
`./hivemindctl start`).

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
  **avatar picker** — Auto (palette hash) or one of six hive-bot tiles.
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

- **Dark/light mode**: header-bar moon/sun button toggles
  `gtk-application-prefer-dark-theme`; the choice persists as
  `"dark_mode": true|false` in `~/.config/hivemind/config.json`.
- **Icons** (`data/icons/*.svg`) are 24×24 `currentColor`-stroked SVGs
  bundled into the binary via GResource (`glib-compile-resources`).
  GTK's pixbuf loader doesn't resolve `currentColor` from CSS, so
  `themed_icon_pixbuf()` string-replaces it with the theme foreground
  (`#eeeeec` dark / `#241f31` light) before rendering — that is what
  makes icons follow the theme.
- **Avatars** (`data/avatars/*.svg`) are hexagonal hive-bots, one per
  `palette_index(name)` color plus `user.svg`. They are self-contained
  (identity colors, dark visor + white eyes) so they read on both themes.
  When the roster row sets an `avatar` field (`avatar-0`..`avatar-5`), the
  matching web SVG (`web/dist/avatars/`) is used instead of the auto
  palette — same id space across client and web.

## Right-click agent menu

Add Task… · **Skills…** (Phase 14.2: shared-library switches + read-only
learned-skills list) · **Inspector…** (Phase 14.4: agent details + container
status with Start/Stop) · Edit Agent · Delete Agent.

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

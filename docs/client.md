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
- **+ Agent:** name, role, model (blank = store default), workspace mode
  (`shared` = container, `host` = none), and the require-approval switch.
- **+ Thread:** pick one or more participants (multi-select = group thread).
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

## Notes for contributors

- `ApiClient` calls are blocking by design — always invoked on detached
  worker threads, results marshalled to the GTK loop through a
  `Glib::Dispatcher` queue (`run_async`). Never block the main loop.
- No WebSocket yet: polling covers messages, activity and approvals.
- The client is pure UI: no containers, no credentials, no agent state.

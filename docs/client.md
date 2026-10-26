# hive-client — GTK4/gtkmm4 native app

The client design is solid and carries over from v1 unchanged in shape:
it talks ONLY to the hive-server hub over HTTPS (Bearer key from
`~/.config/hivemind/config.json`).

## What it shows

- **Roster** — agents grouped by their hive-worker, with presence dots.
  Online workers sort first and start expanded; offline workers sort last,
  start collapsed, and render greyed out — click the group header to
  expand/collapse. Agents on an offline worker are dimmed.
- **Threads** — DM and group threads; chat with streaming replies.
- **Chat images** — image attachments (png/jpg/jpeg/gif/webp) render
  inline, scaled to 480px, click for full-size; downloaded via the hub
  into `~/.cache/hivemind-client/attachments/<thread_id>/` (cached — a
  thread re-open is offline-fast). Agents attach with their `show_image`
  tool; user uploads with image extensions render too. Phase 35 added
  `attach_file`: any workspace artifact (videos included) arrives as a
  chat attachment — worker-routed downloads pass `?worker=<name>`.
- **Theming** — no in-app Dark/Light switch: the client follows the
  GNOME system style live (Phase 37). Ubuntu drives dark via
  `gtk-theme='Yaru-dark'` while `color-scheme` stays 'default', so BOTH
  `org.gnome.desktop.interface` keys are watched. Non-GNOME desktops
  keep GTK's default.
- **Video** (Phase 34 follow-up): video attachments show a thumbnail +
  play/download affordance (stream from the hub or fetch and play
  locally). Rendered at a pinned 640x360 (16:9), click toggles
  play/pause, "open in player" hands off to the system player.
- **Audio** — audio attachments (mp3/flac/wav/ogg/m4a/aac/opus/wma)
  render as a card: big round play/pause button + filename + size,
  playing in place via Gtk::MediaFile (GStreamer). On a media error the
  card falls back to a chip that opens the file externally.
- **Agent management** — create/edit agents, choose their runtime and
  model, inspect execution history, and migrate them between workers.
  Tool calls execute autonomously; there is no approval inbox.
- **Skills page** — browse the hub's library, install from the registry,
  per-agent enable toggles.
- **Computer use** — watch an agent's browser live (worker-hosted
  browser containers), take over logins, hand control back.

## Internals worth knowing

- Poll loop: incremental `?after=<msg_id>` message fetch + activity
  snapshot; dedupe by message id; bubbles render once (no full
  re-render per poll).
- Async: network on worker threads (`run_async`), UI updates posted to a
  main-thread queue; widget-lifetime guards on every deferred touch.
- UiApi (`HIVEMIND_UI_PORT`, default 8481): headless control for tests —
  see client_api.md.
- The client never sees containers, MCP, or LLM credentials — those live
  on the worker side.

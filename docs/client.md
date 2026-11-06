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
- **Integrated voice** — this is part of the same `hive-client` process and
  binary; no standalone desktop voice application remains. Selecting an agent
  opens or reuses its DM. When the configured ChimpFlow gateway and API key
  are available, the left sidebar
  switches to the voice session while the DM remains visible on the right.
  **Back** restores Agents/Threads without losing the DM; selecting the same
  agent again re-enters voice. If ChimpFlow is unavailable, agent selection
  keeps the ordinary DM behavior. Hub connection settings come from the main
  client; existing voice preferences and per-agent speaker assignments are
  preserved. **Super+Alt+Backspace**
  activates the same Talk/Send button through the XDG GlobalShortcuts portal.
  The key is bound only while this voice page is open; Back, thread changes,
  and window close destroy the portal session, so ordinary chat has no global
  binding. The portal may request permission on first use. See
  [Voice global shortcuts on GNOME/Wayland](voice-global-shortcuts.md) for the
  portal contract, giomm callback-order warning, accessibility rules, and
  diagnostic runbook. The voice page also shows the shortcut and a live
  ChimpFlow Online/Offline status card. Agent busy/ready state comes from the
  main window's existing roster snapshots, not a second voice-side roster
  poll; a shared ready transition restores the idle sphere and Talk control,
  hides Cancel, and drains any remaining narration before completing.
- **Message narration** — readable agent bubbles have a small Play button.
  It reads that exact message through ChimpFlow with the sender's configured
  voice, including messages in the Inbox. Clicking another bubble interrupts
  the previous narration; long messages are queued in UTF-8-safe speech
  chunks, and markup, links and code are cleaned for speech.
- **Chat images** — image attachments (png/jpg/jpeg/gif/webp) render
  inline, scaled to 480px, click for full-size; downloaded via the hub
  into `~/.cache/hivemind-client/attachments/<thread_id>/` (cached — a
  thread re-open is offline-fast). Agents attach with their `show_image`
  tool; user uploads with image extensions render too. Phase 35 added
  `attach_file`: any workspace artifact (videos included) arrives as a
  chat attachment — worker-routed downloads pass `?worker=<name>`.
  Attachment-only messages (no caption) render their attachments normally.
- **Files and uploads** — non-media attachments show a filename/size chip
  with open and Save-to-Downloads actions. Images larger than 1024 px on
  their longest edge are downscaled proportionally before upload; smaller
  images and non-image files keep their original bytes. Picking a file
  displays an animated prepare/upload row until the transfer succeeds or
  reports an error; the blocking transport does not expose byte-level
  percentage progress.
- **Theming** — no in-app Dark/Light switch: the client follows the
  GNOME system style live (Phase 37). Ubuntu drives dark via
  `gtk-theme='Yaru-dark'` while `color-scheme` stays 'default', so BOTH
  `org.gnome.desktop.interface` keys are watched. On GTK 4.20+ the client
  selects Adwaita's variant through process-local `GtkSettings`; it never
  writes the desktop's GSettings. Matching `GtkCssProvider` properties govern
  the application's own CSS media queries. GNOME high contrast is watched
  through `org.gnome.desktop.a11y.interface`; the older prefer-dark property
  is only a compatibility fallback. Adwaita is selected explicitly for
  consistent GTK4 styling across Ubuntu's Yaru installations. The embedded
  voice page inherits the same window colors rather than loading a separate
  dark theme. Primary icon-only controls have explicit AT-SPI accessible
  names. Non-GNOME desktops keep GTK's default appearance detection.
- **Video** (Phase 34 follow-up): video attachments show a centred
  640x360 inline player with a red play overlay (never autoplayed).
  Click toggles play/pause. Download and External player actions sit side
  by side below the player; the latter uses the desktop's default app.
  Paused cards are cached first-frame posters. One persistent GStreamer
  `playbin` + `gtk4paintablesink` supplies the active card's paintable;
  track changes follow READY → new URI → PLAYING. This bounds FD/GL use
  instead of constructing a decoder for every attachment.
- **Audio** — audio attachments (mp3/flac/wav/ogg/m4a/aac/opus/wma)
  render as a card: big round play/pause button + filename + size,
  opens the file in the desktop's assigned media player (no in-process
  audio decoder).
- **Agent management** — create/edit agents, choose their runtime and
  model, inspect execution history, and migrate them between workers.
  Tool calls execute autonomously; there is no approval inbox.
- **Skills page** — browse the hub's library, install from the registry,
  installation for every agent; agents own their workspace/private skills.
- **Computer use** — watch an agent's browser live (worker-hosted
  browser containers), take over logins, hand control back.

## Internals worth knowing

- Poll loop: incremental `?after=<msg_id>` message fetch + activity
  snapshot; dedupe by message id; bubbles render once (no full
  re-render per poll).
- Async: network on worker threads (`run_async`), UI updates posted to a
  main-thread queue with a guaranteed final dispatcher wake. Shared async
  state owns the API client, queue and dispatcher so long requests can finish
  after window closure without touching destroyed widgets; closing disconnects
  the UI handler and drops pending UI jobs. API connection settings are
  mutex-snapshotted per request. Agent-option loading, ownership checks,
  skill creation and task saves stay off the GTK thread. Widget-lifetime
  guards cover deferred touches.
- UiApi (`HIVEMIND_UI_PORT`, default 8481): headless control for tests —
  see client_api.md.
- The client never sees containers, MCP, or LLM credentials — those live
  on the worker side.

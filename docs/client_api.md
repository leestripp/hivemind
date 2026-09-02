# hive-client UiApi — embedded REST test surface

Phase 11.5 (2026-08-23). hive-client embeds an OmniNet HTTP server so the
GTK UI can be inspected and driven end-to-end from scripts, tests, and
agents — no display interaction required.

**All action endpoints marshal onto the GTK main loop and drive the exact
same code paths as the window's buttons.** There is no parallel "test
mode" logic; if it works through the API, it works in the UI.

## Binding & configuration

| | |
|---|---|
| Address | `127.0.0.1` only (never exposed) |
| Default port | `8481` |
| Override | `HIVEMIND_UI_PORT=<port>` |
| Disable | `HIVEMIND_UI_PORT=0` or `off` |

The API starts after the main window is presented (`HiveApplication::
on_activate`) and stops on application shutdown. No authentication — it
is localhost-only by design; do not change the bind address.

## Endpoints

### GET /ui/health
Liveness probe.

```json
{"app": "hive-client", "ok": true}
```

### GET /ui/state
What the window currently shows/knows (GTK-thread snapshot).

```json
{
  "connected": true,
  "current_thread": "cf0b48a8-…",
  "busy": false,
  "pending_approvals": 0,
  "status": "Connected — 1 agents, 2 threads",
  "agents":  [ { "id": "…", "name": "Bob", … } ],
  "threads": [ { "id": "…", "title": "…", "kind": "dm", … } ],
  "rendered_count": 4
}
```

- `status` — the literal status-bar text (poll errors surface here).
- `rendered_count` — number of chat bubbles actually on screen. This is
  the key assertion for "did the UI render it" tests.

### GET /ui/messages
The bubbles actually rendered in the chat view, in order.

```json
{ "messages": [
  { "sender_kind": "user",  "sender_label": "You", "body": "…", "created_at": "…" },
  { "sender_kind": "agent", "sender_label": "Bob", "body": "…", "created_at": "…" }
] }
```

### POST /ui/action/new-thread
Body: `{"title": "…", "agent_ids": ["<agent-id>", …]}` → `201` thread JSON.

Same path as the "+ Thread" dialog's Create button. Always creates a new
thread (never DM-deduped — see below).

### POST /ui/action/select-thread
Body: `{"id": "<thread-id>"}` → `{"selected": "<id>"}` or `404`.

Same as clicking a thread row: switches the view, clears the transcript,
kicks a poll. Errors if the id is not in the loaded thread list.

### POST /ui/action/send
Body: `{"body": "…"}` → `202 {"sent": true, "thread": "<id>"}`.

Same as the Send button: fills the composer entry and fires `on_send()`
(posts to the selected thread, async agent turns). `400` if no thread is
selected or body empty.

### POST /ui/action/stop
→ `{"cancel_requested": true}`. Same as the Stop button: `POST
/api/threads/:id/cancel` on the selected thread (cancels live agent turns
server-side). `400` if no thread selected.

### POST /ui/action/delete-thread
Body: `{"id": "<thread-id>"}` → `{"deleted": true}`.

Same end state as the Delete-thread button's confirm dialog (the dialog
itself is UI chrome; the API performs the delete + view cleanup directly).
Clears the view if the deleted thread was selected; refreshes the list.

## Headless end-to-end recipe

```bash
# 1. Xvfb FIRST, and verify it is actually accepting connections —
#    GTK fails with "Failed to open display" otherwise (client stays
#    alive but headless, UiApi never starts).
Xvfb :99 -screen 0 1280x800x24 &
DISPLAY=:99 xdpyinfo >/dev/null || exit 1

# 2. Launch (single-instance app — kill any running hive-client first)
setsid dbus-run-session -- env GDK_BACKEND=x11 DISPLAY=:99 \
    ./build/hive-client/hive-client &

# 3. Drive it
curl -s http://127.0.0.1:8481/ui/health
curl -s -X POST -H 'Content-Type: application/json' \
     -d '{"title":"t","agent_ids":["<agent-id>"]}' \
     http://127.0.0.1:8481/ui/action/new-thread
curl -s -X POST -H 'Content-Type: application/json' \
     -d '{"body":"ping"}' http://127.0.0.1:8481/ui/action/send
curl -s http://127.0.0.1:8481/ui/messages    # poll until the reply renders
```

### Gotchas (learned the hard way)

- **`json{vec}` double-wraps** — nlohmann brace-init on a vector makes
  `[[...]]`. Use `json(vec)`. `/ui/messages` had this bug.
- **`pkill  hive-client` kills your own launcher shell** (its cmdline
  contains the string). Use `pkill -x hive-client`.
- **`GDK_BACKEND=x11` is required under Xvfb** — the shell environment
  forces wayland.
- **GApplication is single-instance** — a stale running client means your
  new launch exits immediately; always `pkill -x hive-client` first.
- **DM dedup is caller-scoped**: server-side `POST /api/threads` reuses an
  existing 1:1 thread only when the request has `"reuse_dm": true`
  (sent by the roster click, NOT by New Thread or this API's new-thread).

(The `/ui/action/theme` endpoint was removed in Phase 37 — the client
now follows the GNOME system style and has no in-app toggle.)

## v2 note

The UiApi is unchanged in v2 — it drives the local client UI for headless
tests (xvfb). The client's SERVER-side API surface also keeps its v1
shape (threads/messages/roster); only the hub's internals changed.

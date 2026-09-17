# Web UI

A thin read-mostly web client served by the hub itself (`web/dist`
static files). Same threads/messages/roster API as the GTK client; used
for quick checks from machines without the native app.

Scope: threads, messages (with inline images via the download
endpoint's real MIME types), roster status, worker list. Editing actions
(model config, container ops) are minimal — the GTK client is the primary
UI. The web UI never gains agent-hosting features; the hub has none.

# Web administrative console

The hub serves a static ES-module application from `web/dist`. It is an
**administrative console**, not a thread/chat client and not an agent host.
For conversation interfaces use the GTK desktop client, Android app, or
native GTK voice client. No `HiveOffice` component exists in this source
tree.

## Pages and capabilities

Current navigation is **Agents, Tasks, Files, Skills, Secrets, Models,
Workers, System, Keys, Settings**:

- create/edit/migrate agents, inspect activity and execution state, and
  start/stop an agent's noVNC screen;
- create/run/enable/delete routines and clean up routine history;
- browse/download worker files and inspect shared workspaces;
- manage the skill library, installation, deletion, and per-agent grants;
- manage secret metadata, grants, and revocation without reading values back;
- manage hub-owned providers/models, discovery, and the default model;
- inspect/enroll/remove workers;
- inspect system resource/history/agent status;
- mint/revoke/reactivate/delete API keys; and
- manage connectors, message-window configuration, theme/timezone, health,
  and hub settings.

This UI has substantial write capability. The old description of a
read-mostly threads/messages/roster client is obsolete; current web code
has no chat composer or message thread page.

## Stack and authentication

`web/dist/js/api.js` wraps `fetch` with JSON/error handling and request-id
display. Pages are plain JavaScript modules with shared CSS; there is no
browser build step. The hub registers API routes before its static
catch-all, so `web/dist` must remain the last route.

The browser prompts for a bearer key and persists it in `localStorage`.
That key authorizes the admin operations above, so protect the origin from
XSS and untrusted static/dependency changes. Use an appropriately scoped
key where one is sufficient. The web UI never joins the worker channel and
never runs agents, models, containers, or MCP servers itself.

## Changing the console

Keep modules small, use the shared API/error helpers, and update any cache
buster referencing an edited module. Run `node --check` on changed
JavaScript files and exercise write flows against the real hub routes;
static inspection alone does not prove route payload compatibility.

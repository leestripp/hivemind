# Computer use (v2)

Agents that need a real browser (logins, JS-heavy sites, captchas a human
must pass) get one on their WORKER: an on-demand desktop stack in the
agent's own container (Xvfb + x11vnc + websockify/noVNC + Chromium under
openbox), started via `screen_ctl start` → in-container
`hive-screen.sh`.

Phase 36 made desktops on-demand: every container publishes loopback
6080 at create time, so starting the screen is a pure `podman exec` (no
container recreate — the agent brain survives). When no viewer is
connected for HIVEMIND_SCREEN_IDLE_MIN (default 15) minutes, the
worker's idle watcher tears the stack down, reclaiming ~350–700 MB of
RAM per idle agent. `hive-screen.sh stop` kills the x11vnc watchdog
first (pidfile) so it can't revive the stack mid-teardown.

- The browser profile persists per agent (on its home volume), so
  sessions survive restarts.
- The client shows the live screen (Computer button) and can take over:
  the same browser session is shared, so the human logs in and hands
  control back to the agent.
- The hub only routes the stream/control messages; the pixels never
  leave the worker except as the viewer stream to the client.

Implementation detail from v1 that carries over: the browser container
runs on the same podman host as the agent's container and shares its
network namespace, so the agent's browser automation tools connect
directly (localhost CDP) — in v2 the agent IS in that container, making
this even simpler.

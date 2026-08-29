# Computer use (v2)

Agents that need a real browser (logins, JS-heavy sites, captchas a human
must pass) get one on their WORKER: a browser container running
Chromium + a VCD/noVNC-style stream, managed by the worker's fleet layer
like any other container.

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

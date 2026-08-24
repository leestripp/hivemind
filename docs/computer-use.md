# Agent Computer — live screen (Phase 25, X11 stack)

> **Status: WORKING — user-verified 2026-08-24** in both the web UI and the
> GTK client (Phase 26 WebKitGTK embed). This document is the definitive
> reference for how it works and how it was made to work — including the
> failed GNOME/Wayland detour (Phase 24) so nobody repeats it.

## What it is

Every containerized agent can expose a live desktop you can watch and
drive from the browser ("🖥 Computer" on the web agents page / client
inspector). The primary use is **login handoff**: the agent browses with
its own chromium, hits a login wall, you take over the same screen, log in,
and hand control back — the profile persists, so the session survives.

## Architecture (the working stack)

```
┌─ your browser ─────────────────────────────────────────────┐
│  noVNC (vnc.html?autoconnect&path=/screen/ws?agent=<id>    │
│                          &password=<pw>)                   │
└──────────┬──────────────── WebSocket ──────────────────────┘
           ▼
┌─ hive-server :8421 ────────────────────────────────────────┐
│  GET /screen/<id>/    → 302 → vnc.html (proxied assets)    │
│  GET /screen/ws?agent=<id> → WS upgrade → raw-TCP splice   │
│  Auth: Bearer / ?key= / HttpOnly cookie (hive_screen)      │
└──────────┬──────────────── 127.0.0.1:<published> ──────────┘
           ▼
┌─ podman container (hive-<agent-id>, user hive) ────────────┐
│  websockify :6080 ──▶ x11vnc :5900 ──▶ Xvfb :99 (1280x800) │
│                          ▲                    ▲            │
│                    watchdog loop          openbox (WM)     │
│                                           chromium (max)   │
│                                           └─ CDP :9222 ◀── browser_* tools
└────────────────────────────────────────────────────────────┘
```

The same chromium the user sees is the one the agent's `browser_*` tools
drive — they attach over CDP :9222 (`browser_driver.py` launches it with
`--remote-debugging-port=9222` if it isn't running). One browser, two
drivers (human and agent).

## The components, and why these exact ones

| Component | Choice | Why |
|---|---|---|
| Display | **Xvfb** `:99 -screen 0 1280x800x24` | virtual framebuffer; no GPU/session needed |
| Window manager | **openbox** | the Phase 6.1 black screen was an EMPTY DISPLAY — no WM, nothing drawn. openbox is tiny and bulletproof in containers. (Anthropic's computer-use demo uses mutter as its WM; mutter's standalone X11 mode is gone in GNOME 50.) |
| VNC server | **x11vnc** `-forever -shared -passwd <pw> -localhost -wait 50` | scrapes the Xvfb display; `-forever -shared` keeps it up across client drops; plus a 5s watchdog loop restarts it if it dies on odd X events (Anthropic recipe) |
| WS bridge + web | **websockify** `--web /usr/share/novnc 6080 localhost:5900` | serves the noVNC assets AND bridges WS→TCP |
| Browser | **Google Chrome stable** (official .deb) | the real production browser — no testing branding, and sites that reject automation/test builds at sign-in (Google) accept it, which the login-handoff flow needs. Ubuntu's `chromium-browser` deb is a **snap stub** (snaps can't run in containers); playwright's bundled build is *Chromium for Testing* (branded, and gets flagged). Playwright (PyPI wheel — the Ubuntu deb strips the node driver) remains the DRIVER, its chromium the last-resort fallback. Stale `Singleton{Lock,Socket,Cookie}` files are cleared before launch — the profile persists in the home mount across container recreates |
| noVNC client | Ubuntu `novnc` package assets | vnc.html takes `?autoconnect&path=&password=` query params |

## The three bugs that made it look broken

1. **Path doubling** (fixed `fdb7fa5`): vnc.html's `path=` param was passed
   *relative* (`screen/ws?agent=…`); noVNC resolves it against the page URL
   (`/screen/<id>/vnc.html`) → WS to `/screen/<id>/screen/ws` → 404.
   Symptom: "opens but can't connect". Fix: percent-encoded ABSOLUTE path
   (`%2Fscreen%2Fws%3Fagent%3D<id>`).
2. **Black screen**: display :99 had nothing on it — no WM, and chromium
   never launched (snap stub). Symptom: connected, black, user bails.
   Fix: openbox + playwright chromium `--start-maximized` at bring-up.
3. **Empty `password=` in the redirect**: `screen_password()` read the
   password file host-side, but the `:U` bind mount chowns the agent home
   to the container uid — permission denied silently read as "". Then a
   repair introduced an inverted trim (`!isspace` popped the whole
   password). Fix: read via `podman exec`, trim `isspace` correctly.

## Passwords

Per-agent, generated once by `hive-screen.sh` into the home mount
(`/home/hive/.vnc-password`, 0600), **exactly 8 chars — RFB truncates at
8**. `ContainerManager::screen_password()` returns it via `podman exec
cat`; `ScreenRoutes` appends it to the noVNC URL as `?password=`. The
screen page is already API-key gated; the VNC password is defense in depth
(localhost-only ports regardless).

## Operating it

```bash
# Rebuild the image (rig):
cd ~/hivemind && podman build -t localhost/hive-agent:browser \
    -f hive-server/data/Containerfile.hive-agent-browser hive-server/data

# Screen lifecycle:
POST /api/agents/:id/screen/start   # ensure_screen: recreate-if-stale → start → hive-screen.sh
POST /api/agents/:id/screen/stop
GET  /screen/<id>/?key=<api-key>    # → noVNC, autoconnects

# Logs INSIDE the container when something's off:
podman exec hive-<id> sh -c 'tail /tmp/{xvfb,openbox,x11vnc,websockify,chromium}.log'
```

`ensure_screen` recreates the container when the **image digest** changed
(names/tags lie — rebuilds keep the same tag). Bring-up is idempotent and
zombie-safe (PID 1 is `sleep`, never reaps; liveness checks read
`/proc/<pid>/stat` state).

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| "opens but can't connect" | `path=` must be absolute (`%2Fscreen%2Fws…`) — fdb7fa5 |
| black screen | nothing on the display — openbox + chrome launch in hive-screen.sh |
| **Chrome "Page Unresponsive" dialogs** | podman's default 64MB /dev/shm starves the renderer — `--shm-size 1g` at container create + `--disable-dev-shm-usage --disable-gpu` launch flags (719b9a9). Note: existing containers keep old flags until recreated — `ensure_screen` only recreates on image-digest change, so `podman rm -f hive-<id>` by hand after a flags-only change |
| Chrome won't start: "profile in use on another computer" | stale `SingletonLock` from a dead container in the persistent profile — cleared automatically before launch (7236649) |
| "Chromium for Testing" branding / site sign-in refused | playwright's build is a test build — the image ships real Google Chrome stable (8d51320) |
| no sound (YouTube etc.) | by design — RFB/VNC carries framebuffer + input only, no audio channel. Not a bug; container audio streaming (PulseAudio-over-TCP) is possible but deliberately not wired |

## Test with a REAL client, always

Raw TCP/curl probes are pathological RFB clients and misled us for hours
(GRD's throttler, connect-and-close poisoning session state, false
"server broken" readings). The browser IS the test: open the screen URL,
check `document.getElementById('noVNC_status').textContent`, sample the
canvas pixels. One real client connection answers more than fifty probes.

---

## Appendix: Phase 24 — the GNOME/Wayland detour (failed, kept for reference)

Tried to give agents a real GNOME desktop on Wayland. All of this was
TRUE and none of it was fixable in a day:

- **Ubuntu 26.04 GNOME 50 is Wayland-only** — the X11 session was removed
  upstream. (User instinct was right; the container is the constraint.)
- **gnome-shell cannot run in a container** — init hard-requires logind
  (`LoginManagerSystemd`, fatal without `org.freedesktop.login1`).
- **mutter --headless WORKS** — GNOME's compositor ran fine, GPU-accelerated
  via `--device /dev/dri` (RX 9070, GBM renderer). Kept in git history.
- **Ubuntu builds gnome-remote-desktop with meson `-Dvnc=false`** — rebuilt
  GRD 50.2 from source with VNC on (needed `libei-dev`, `libdbus-1-dev`);
  headless enable lives in `org.gnome.desktop.remote-desktop.vnc.headless`
  (NOT the regular schema); `grdctl ... enable` needs systemd — set
  gsettings directly. Daemon ran, "VNC server started", listened on :5900…
- **…then closed every client before the RFB greeting.** Cause never
  isolated (suspect: per-client PipeWire stream creation from mutter's
  RemoteDesktop API — `grd-session-vnc.c:913`). Abandoned, not mourned:
  the X11 stack does the job with 10% of the moving parts.

If Wayland is ever revisited: start from the daemon with
`G_MESSAGES_DEBUG=all` (copy a runner in via `podman cp` — ssh quoting ate
repeated inline attempts), with a real noVNC client, and read the session
setup logs.

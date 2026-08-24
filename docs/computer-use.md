# Agent Computer (live screen) — status, findings, and what failed

> State as of 2026-08-24: **partially working, not user-ready.** The screen
> page connects and authenticates, the GNOME (mutter, Wayland) headless
> desktop runs GPU-accelerated in the agent container, but the VNC server
> (gnome-remote-desktop) closes every client connection before the RFB
> greeting. Root cause not yet found. This doc is the full trail so the
> next session doesn't repeat it.

## What the feature is

The "🖥 Computer" button (web agents page / client inspector) opens a live
view of the agent's container desktop via noVNC, proxied through hive-server:

```
browser noVNC ──WS──▶ hive-server /screen/ws (auth, splice)
                          │ raw TCP
                    container :6080 websockify ──▶ :5900 VNC server ──▶ desktop
```

Auth: bearer key / `?key=` / HttpOnly cookie. Container's :6080 is published
on 127.0.0.1 only; hive-server is the single entry point.

## What WORKS (verified on the rig)

- **Deployment rig "dev-rig"** (AerynOS 26.03, RX 9070): hive-server as
  systemd service (`hive-server.service`, user `hivemind`), rootless podman
  fully working (the dev sandbox's limitation is gone). Deploy loop:
  `git push rig main && ssh root@dev-rig /usr/local/bin/hive-deploy`.
- **WS splice + page proxy**: noVNC page loads, websocket upgrades, binary
  subprotocol negotiated. (Fixed `fdb7fa5`: vnc.html `path=` param must be
  ABSOLUTE — `/screen/ws` — or noVNC resolves it against `/screen/<id>/`
  and the WS 404s. Symptom was "opens but can't connect".)
- **mutter --headless**: runs clean in the container, `--virtual-monitor
  1280x800`, hardware GL via `--device /dev/dri` passthrough (GBM renderer
  on the RX 9070, "high priority EGL context").
- **gnome-remote-desktop 50.2 headless VNC daemon**: registers on the
  session bus, "VNC server started", listens on :5900 (v4+v6).
- **VNC password plumbing**: per-agent 8-char password generated into the
  home mount, handed to noVNC via `?password=` (a supported noVNC query
  param per its EMBEDDING.md), read server-side via `podman exec` (the
  direct host-file read fails: the `:U` bind mount chowns the home dir to
  the container uid — permission denied, not missing).

## What FAILED / current blocker

**GRD accepts the TCP connection then closes it before the RFB greeting.**
Raw in-container probe: connect to 127.0.0.1:5900 succeeds, read returns
EOF instantly. Through websockify this surfaces as a WS close frame with
reason "Target closed" (code 1000); noVNC shows a black frame then drops
back to the Connect screen.

Ruled out:
- *Throttler lockout* — GRD rate-limits (10 attempts/s/peer, 5 pending),
  and my dozens of raw probes did trip it, but a fresh container +
  single clean client connection fails identically.
- *Password/auth* — server closes before any greeting; RFB auth happens
  after. Not auth.
- *v4-vs-v6* — 5900 answers on both.

Suspected (unverified): GRD's VNC session creates a PipeWire stream from
mutter's RemoteDesktop API per client; if stream/session creation fails the
client is dropped silently (grd-session-vnc.c:913 "PipeWire stream closed,
closing client"). PipeWire + wireplumber RUN in the container but with
degraded warnings (no system bus for rtkit, no systemd, no snapd). The
daemon logs nothing at default verbosity on the failure — next debug step
is `G_MESSAGES_DEBUG=all` on the daemon via a script copied in with
`podman cp` (ssh+podman-exec quoting kept eating the inline attempts).

## Things discovered the hard way (don't re-learn)

1. **Ubuntu 26.04 GNOME 50 is Wayland-only** — the X11 session was removed
   upstream. Xvfb+x11vnc was a dead end for a GNOME desktop anyway.
2. **gnome-shell CANNOT run in a container**: its init hard-requires
   logind (`LoginManagerSystemd` — fatal Gio error without
   org.freedesktop.login1). mutter --headless is GNOME's compositor and
   needs nothing of the sort. No Shell chrome (top bar/Activities) — the
   desktop surface is the app (chromium maximized), which is all the
   Agent Computer needs.
3. **Ubuntu ships gnome-remote-desktop built with meson `-Dvnc=false`**
   (the upstream default) — grdctl has NO vnc subcommand. The image
   rebuilds GRD 50.2 from the upstream tarball with `-Dvnc=true
   -Drdp=false -Dsystemd=false`, installed `--prefix=/usr` over the deb.
   Build deps that bit us: `libei-dev`, `libdbus-1-dev` (plus the usual
   glib/pipewire/secret/xkbcommon/epoxy/drm/fuse3/tss2/notify/cairo/
   girepository).
4. **GRD headless VNC enable lives in a different gsettings schema**:
   `org.gnome.desktop.remote-desktop.vnc.headless` — writing the
   non-headless schema does nothing (grdctl status stays "disabled").
   And `grdctl ... enable` needs systemd — set gsettings directly.
5. **RFB truncates VNC passwords at 8 chars.** Longer fails GRD's
   set-password ("Password is too long").
6. **Ubuntu's `chromium-browser` deb is a snap stub** — snaps can't run in
   containers. The image uses playwright's bundled chromium (PyPI wheel —
   Ubuntu's python3-playwright deb strips the node driver, cli.js, so
   `playwright install` fails there; pip `--break-system-packages` works,
   browsers in `PLAYWRIGHT_BROWSERS_PATH=/opt/ms-playwright`, world-
   readable — build runs as root, agents run as hive).
7. **Container PID 1 is `sleep`** — it never reaps, so dead processes
   linger as zombies and `pgrep` matches them. hive-screen.sh uses
   zombie-aware liveness checks (`/proc/<pid>/stat` state != Z).
8. **dbus-run-session tears the bus down when its main child exits** —
   keep the session script alive with `sleep infinity`; background the
   GRD daemon.
9. **pgrep  self-match**: a probe shell whose own cmdline contains the
   pattern kills itself (`pkill  gnome-remote-desktop-daemon`). Also:
   pkill without  can't match names >15 chars. Use `[n]`-style regex
   tricks or PID lists.
10. **Backgrounded builds over ssh die with the channel** — use
    `systemd-run --uid=… --unit=… --collect` on the rig for image builds.

## Files in the current (partially working) implementation

- `hive-server/data/Containerfile.hive-agent-browser` — mutter, GRD source
  build, pipewire/wireplumber, xwayland, novnc/websockify, pip playwright
- `hive-server/data/hive-screen.sh` — idempotent bring-up (all the lessons
  above are in its header comments)
- `hive-server/src/ContainerManager.cpp` — ensure_screen (digest-based
  recreate, /dev/dri passthrough, runs hive-screen.sh), screen_password
- `hive-server/include/hivemind/routes/ScreenRoutes.cpp` — page proxy +
  WS splice + absolute-path/password redirect params
- `hive-server/data/browser_driver.py` — CDP attach/launch of the shared
  chromium (Wayland env), ~/.browser-profile persists logins

## Next steps (when we pick this back up)

1. **Test with a REAL client first** — the stack has only ever been probed
   with raw TCP/curl, which are pathological RFB clients. Open the screen
   URL in a browser (noVNC, real client, password flows via ?password=):
   `http://dev-rig:8421/screen/<agent-id>/?key=<api-key>`.
2. If it still drops: run the GRD daemon with `G_MESSAGES_DEBUG=all`
   (copy a runner script in via `podman cp`, don't fight ssh quoting) and
   watch session/stream creation on connect.
3. If PipeWire-in-container is the wall: consider cutting GRD out of the
   loop — mutter alone + `wayvnc` won't work (mutter isn't wlroots), so
   the fallback is back to Xvfb+x11vnc (works, it's what shipped in Phase
   6.1, just not GNOME) OR RDP + a web RDP client (Guacamole — heavy).

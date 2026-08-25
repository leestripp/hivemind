#!/bin/bash
# hive-screen.sh — bring up the X11 screen stack inside a
# hive-agent:browser container (Phase 25). Idempotent: safe on every
# screen open. Layout: Xvfb :99 → openbox (WM) → x11vnc :5900 (password)
# → websockify/noVNC :6080 → chromium maximized (shared CDP browser).
#
# Lessons baked in (docs/computer-use.md):
#  - PID 1 (sleep) never reaps → zombie-aware liveness checks.
#  - RFB truncates VNC passwords at 8 chars.
#  - chromium is playwright's bundled build (the deb is a snap stub).
#  - x11vnc gets a watchdog loop (Anthropic's demo recipe): it can die on
#    odd X events; restart it so the screen never goes dark mid-session.
set -u

export DISPLAY=:99

# Per-agent VNC password (8 chars — RFB limit), in the home mount.
PW_FILE="/home/hive/.vnc-password"
_pw="$(cat "$PW_FILE" 2>/dev/null || true)"
if [ "${#_pw}" -ne 8 ]; then
    head -c 4 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$PW_FILE"
fi
chmod 600 "$PW_FILE"
PW="$(cat "$PW_FILE")"

alive() {  # alive <exact-process-name> — zombies don't count
    for p in $(pgrep -x "$1" 2>/dev/null); do
        [ "$(cut -d' ' -f3 /proc/$p/stat 2>/dev/null)" != "Z" ] && return 0
    done
    return 1
}

RES="${1:-1280x800}"   # Phase 28: server passes ServerConfig screen_resolution
alive Xvfb || nohup Xvfb :99 -screen 0 ${RES}x24 >/tmp/xvfb.log 2>&1 &
sleep 1
alive openbox || nohup openbox >/tmp/openbox.log 2>&1 &

# x11vnc + watchdog (restarts it if it dies)
if ! alive x11vnc; then
    nohup x11vnc -display :99 -forever -shared -passwd "$PW" \
        -localhost -wait 50 >/tmp/x11vnc.log 2>&1 &
    nohup bash -c 'while true; do
        pgrep -x x11vnc >/dev/null || exec x11vnc -display :99 -forever \
            -shared -passwd "'"$PW"'" -localhost -wait 50 \
            >>/tmp/x11vnc.log 2>&1
        sleep 5
    done' >/dev/null 2>&1 &
fi

alive websockify || \
    nohup websockify --web /usr/share/novnc 6080 localhost:5900 \
        >/tmp/websockify.log 2>&1 &

# The shared browser: maximized on :99, CDP :9222 for browser_driver.py.
# Skip if already up (CDP answers).
if ! curl -sf --max-time 2 http://127.0.0.1:9222/json/version >/dev/null 2>&1; then
    # Real Chrome first (no testing/automation branding — sites that
    # reject test builds must accept it); playwright chromium as fallback.
    # Stale singleton locks survive container recreates (home mount
    # persists) and block Chrome: "profile in use on another computer".
    # Safe to clear here — this branch only runs when CDP is DOWN, i.e.
    # no live chrome holds the profile.
    rm -f /home/hive/.browser-profile/Singleton{Lock,Socket,Cookie}
    CHROME="$(command -v google-chrome || ls /opt/ms-playwright/chromium-*/chrome-linux64/chrome /opt/ms-playwright/chromium-*/chrome-linux/chrome 2>/dev/null | tail -1)"
    [ -n "$CHROME" ] && nohup "$CHROME" --remote-debugging-port=9222 \
        --user-data-dir=/home/hive/.browser-profile --no-first-run \
        --no-default-browser-check --disable-session-crashed-bubble \
        --disable-dev-shm-usage --disable-gpu --noerrdialogs \
        --disable-features=TranslateUI \
        --start-maximized about:blank >/tmp/chromium.log 2>&1 &
fi

# Readiness: x11vnc + websockify alive and :5900 answering.
for _ in $(seq 1 30); do
    alive x11vnc && alive websockify && \
    (exec 3<>/dev/tcp/127.0.0.1/5900) 2>/dev/null && exit 0
    sleep 0.5
done
echo "hive-screen: stack did not come up — see /tmp/*.log" >&2
exit 1

#!/bin/bash
# hive-screen.sh — bring up the X11 screen stack for an agent
# (Phase 25: containers; Phase 72.2: bare-metal host agents).
# Idempotent: safe on every screen open. Layout: Xvfb :99+SLOT →
# openbox (WM) → x11vnc :5900+SLOT (password) → websockify/noVNC
# :6080+SLOT → chromium maximized (shared CDP browser :9222+SLOT).
#
# SLOT (Phase 72.2): the container user ("hive") and hivex-queenbee keep
# slot 0 — the legacy :99 / 5900 / 6080 / 9222 stack. Other hivex-<name>
# users derive a deterministic 1..50 from the username so bare-metal
# agents never collide. SAME formula as browser_driver.py::_default_slot
# — change both or neither. HIVE_BROWSER_SLOT / HIVE_BROWSER_DISPLAY /
# HIVE_BROWSER_CDP_PORT override.
#
# Lessons baked in (docs/computer-use.md):
#  - PID 1 (sleep) never reaps → zombie-aware liveness checks.
#  - RFB truncates VNC passwords at 8 chars.
#  - chromium is playwright's bundled build (the deb is a snap stub).
#  - x11vnc gets a watchdog loop (Anthropic's demo recipe): it can die on
#    odd X events; restart it so the screen never goes dark mid-session.
#  - On multi-agent hosts every pkill below is user-scoped (the script
#    runs AS the agent user), so one agent's stop never kills another's.
set -u

_slot() {
    case "$(id -un)" in
        hivex-queenbee) echo 0;;   # legacy stack predates the knob
        hivex-*) id -un | tr -d '\n' | od -An -v -tu1 \
                 | awk '{for(i=1;i<=NF;i++)s+=$i} END{print s%50+1}';;
        *) echo 0;;                # containers + anything else: legacy
    esac
}
SLOT="${HIVE_BROWSER_SLOT:-$(_slot)}"
export DISPLAY="${HIVE_BROWSER_DISPLAY:-:$((99 + SLOT))}"
RFB=$((5900 + SLOT))
WS=$((6080 + SLOT))
CDP="${HIVE_BROWSER_CDP_PORT:-$((9222 + SLOT))}"
WATCHDOG_PID=/tmp/hive-screen.$SLOT.watchdog.pid

# Per-agent VNC password (8 chars — RFB limit), in the agent's home.
PW_FILE="${HIVE_VNC_PW_FILE:-$HOME/.vnc-password}"
_pw="$(cat "$PW_FILE" 2>/dev/null || true)"
if [ "${#_pw}" -ne 8 ]; then
    head -c 4 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$PW_FILE"
fi
chmod 600 "$PW_FILE"
PW="$(cat "$PW_FILE")"

# "password" mode: print the VNC password and exit. The worker's screen
# status path uses this — its sudoers grant covers this script only.
if [ "${1:-}" = "password" ]; then
    echo "$PW"
    exit 0
fi

# Phase 36: stop mode — tear down the whole display stack (idle timeout
# / manual screen-off). Order matters: the x11vnc WATCHDOG dies first,
# else it revives x11vnc within 5s and the screen never goes dark.
if [ "${1:-}" = "stop" ]; then
    if [  "$WATCHDOG_PID" ]; then
        kill "$(cat "$WATCHDOG_PID")" 2>/dev/null || true
        rm  "$WATCHDOG_PID"
    fi
    pkill -xu "$(id -u)" x11vnc 2>/dev/null || true
    pkill -fu "$(id -u)" "websockify.* $WS " 2>/dev/null || true
    # The shared Chromium (CDP port) lives on this X display — the
    # biggest RAM consumer, and dead anyway once Xvfb goes.
    pkill -fu "$(id -u)" "remote-debugging-port=$CDP" 2>/dev/null || true
    pkill -x openbox 2>/dev/null || true
    pkill -x Xvfb 2>/dev/null || true
    exit 0
fi

alive() {  # alive <exact-process-name> — zombies don't count
    # User-scoped: a multi-agent host runs other agents' daemons too.
    for p in $(pgrep -xu "$(id -u)" "$1" 2>/dev/null); do
        [ "$(cut -d' ' -f3 /proc/$p/stat 2>/dev/null)" != "Z" ] && return 0
    done
    return 1
}

RES="${1:-1280x800}"   # Phase 28: server passes ServerConfig screen_resolution
alive Xvfb || nohup Xvfb "$DISPLAY" -screen 0 ${RES}x24 >/tmp/xvfb.$SLOT.log 2>&1 &
sleep 1
alive openbox || nohup openbox >/tmp/openbox.$SLOT.log 2>&1 &

# x11vnc + watchdog (restarts it if it dies)
if ! alive x11vnc; then
    nohup x11vnc -display "$DISPLAY" -rfbport "$RFB" -forever -shared \
        -passwd "$PW" -localhost -wait 50 >/tmp/x11vnc.$SLOT.log 2>&1 &
    nohup bash -c 'while true; do
        up=0
        for p in $(pgrep -xu "$(id -u)" x11vnc 2>/dev/null); do
            [ "$(cut -d" " -f3 /proc/$p/stat 2>/dev/null)" != "Z" ] && up=1
        done
        [ "$up" -eq 0 ] && exec x11vnc -display '"$DISPLAY"' \
            -rfbport '"$RFB"' -forever -shared -passwd "'"$PW"'" \
            -localhost -wait 50 >>/tmp/x11vnc.$SLOT.log 2>&1
        sleep 5
    done' >/dev/null 2>&1 &
    echo $! > "$WATCHDOG_PID"   # stop kills it first
fi

if ! pgrep -fu "$(id -u)" "websockify.* $WS " >/dev/null 2>&1; then
    nohup websockify --web /usr/share/novnc "$WS" "localhost:$RFB" \
        >/tmp/websockify.$SLOT.log 2>&1 &
fi

# The shared browser: maximized on this display, CDP port for
# browser_driver.py. The wrapper owns the flags (incl. --no-sandbox)
# and the stale-lock dance, and doubles as the desktop menu's relaunch
# entry — one source of truth so the user-relaunched browser matches
# the agent's.
HIVE_BROWSER_CDP_PORT="$CDP" hive-chrome.sh || true

# Readiness: x11vnc + websockify alive and the RFB port answering.
for _ in $(seq 1 30); do
    alive x11vnc && pgrep -fu "$(id -u)" "websockify.* $WS " \
        >/dev/null 2>&1 && \
    (exec 3<>/dev/tcp/127.0.0.1/$RFB) 2>/dev/null && \
    { echo "PASSWORD=$PW"; exit 0; }
    sleep 0.5
done
echo "hive-screen: stack did not come up — see /tmp/*.$SLOT.log" >&2
exit 1

#!/bin/bash
# hive-screen.sh — bring up the headless GNOME Wayland desktop + VNC +
# noVNC/websockify inside a hive-agent:browser container (Phase 24).
# Idempotent: safe to run on every screen open. All state under /tmp or
# the agent's home mount (persists across container restarts).
set -u

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"

# Per-agent VNC password, generated once (lives in the home mount).
PW_FILE="/home/hive/.vnc-password"
[ -f "$PW_FILE" ] || head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$PW_FILE"
chmod 600 "$PW_FILE"
export HIVE_VNC_PW="$(cat "$PW_FILE")"

GRD_DAEMON=""
for p in /usr/libexec/gnome-remote-desktop-daemon /usr/lib/gnome-remote-desktop-daemon; do
    [ -x "$p" ] && GRD_DAEMON="$p" && break
done

if ! pgrep -x gnome-shell >/dev/null 2>&1; then
    # One dbus session shared by pipewire, the shell, and GRD — that shared
    # bus is how GRD finds the headless session (mutter RemoteDesktop API).
    nohup dbus-run-session -- bash -c '
        pipewire &
        wireplumber &
        gnome-shell --headless --virtual-monitor 1280x800 &
        # shell needs a moment to register on the bus before grdctl works
        for _ in $(seq 1 20); do
            busctl --user list 2>/dev/null | grep -q org.gnome.Shell && break
            sleep 0.5
        done
        printf "%s" "$HIVE_VNC_PW" | grdctl --headless vnc set-password
        grdctl --headless vnc disable-view-only
        grdctl --headless vnc enable
        exec "$0" --headless
    ' "$GRD_DAEMON" >/tmp/gnome-session.log 2>&1 &
fi

# websockify: noVNC web assets + WS→VNC bridge on :6080 (GRD VNC :5900).
pgrep websockify >/dev/null 2>&1 || \
    nohup websockify --web /usr/share/novnc 6080 localhost:5900 \
        >/tmp/websockify.log 2>&1 &

# Readiness: shell up + VNC listening + websockify up (caller polls).
for _ in $(seq 1 40); do
    pgrep -x gnome-shell >/dev/null 2>&1 && \
    pgrep websockify >/dev/null 2>&1 && \
    (exec 3<>/dev/tcp/127.0.0.1/5900) 2>/dev/null && exit 0
    sleep 0.5
done
echo "hive-screen: stack did not come up — see /tmp/gnome-session.log" >&2
exit 1

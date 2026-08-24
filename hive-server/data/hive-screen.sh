#!/bin/bash
# hive-screen.sh — bring up the headless GNOME (mutter) Wayland desktop +
# GRD VNC + noVNC/websockify inside a hive-agent:browser container (Phase 24).
# Idempotent. Session state under the agent's home mount (persists restarts).
#
# Notes from the bring-up research:
#  - gnome-shell CANNOT run here (hard logind dependency, fatal in GNOME 50);
#    mutter --headless is GNOME's compositor and provides the RemoteDesktop
#    D-Bus API GRD needs. The desktop surface is the app (chromium maximized).
#  - GRD headless VNC enable lives in the HEADLESS gsettings schema
#    (org.gnome.desktop.remote-desktop.vnc.headless), not the regular one.
#  - grdctl ... enable needs systemd (absent) — set gsettings directly.
#  - VNC passwords: RFB truncates at 8 chars.
#  - dbus-run-session's bus dies when its main child exits — keep it alive
#    with sleep infinity, background the daemon.
set -u

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/home/hive/.run}"
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"

# Per-agent VNC password, generated once (8 chars — RFB limit), home mount.
PW_FILE="/home/hive/.vnc-password"
[ -f "$PW_FILE" ] || head -c 4 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$PW_FILE"
chmod 600 "$PW_FILE"
export HIVE_VNC_PW="$(cat "$PW_FILE")"

GRD_DAEMON=""
for p in /usr/libexec/gnome-remote-desktop-daemon /usr/lib/gnome-remote-desktop-daemon; do
    [ -x "$p" ] && GRD_DAEMON="$p" && break
done

# Zombie-safe liveness: PID 1 (sleep) never reaps, so dead mutter/daemon
# linger as zombies and plain pgrep would match them.
alive() {  # alive <exact-name>
    for p in $(pgrep -x "$1" 2>/dev/null); do
        [ "$(cut -d' ' -f3 /proc/$p/stat 2>/dev/null)" != "Z" ] && return 0
    done
    return 1
}

if ! alive mutter; then
    nohup dbus-run-session -- bash -c '
        pipewire >/tmp/pipewire.log 2>&1 &
        wireplumber >/tmp/wireplumber.log 2>&1 &
        mutter --headless --virtual-monitor 1280x800 >/tmp/mutter.log 2>&1 &
        # wait for mutter to own the RemoteDesktop API on the session bus
        for _ in $(seq 1 24); do
            busctl --user list 2>/dev/null | grep -q org.gnome.Mutter.RemoteDesktop && break
            sleep 0.5
        done
        printf "%s" "$HIVE_VNC_PW" | grdctl --headless vnc set-password
        gsettings set org.gnome.desktop.remote-desktop.vnc.headless enable true
        "$0" >/tmp/grd.log 2>&1 &
        sleep infinity
    ' "$GRD_DAEMON" >/tmp/session.log 2>&1 &
fi

# websockify: noVNC web assets + WS→VNC bridge on :6080 (GRD VNC :5900).
alive websockify || \
    nohup websockify --web /usr/share/novnc 6080 localhost:5900 \
        >/tmp/websockify.log 2>&1 &

# Readiness: mutter + VNC listening + websockify.
for _ in $(seq 1 48); do
    alive mutter && alive websockify && \
    (exec 3<>/dev/tcp/127.0.0.1/5900) 2>/dev/null && exit 0
    sleep 0.5
done
echo "hive-screen: stack did not come up — see /tmp/session.log /tmp/grd.log" >&2
exit 1

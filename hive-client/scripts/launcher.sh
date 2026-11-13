#!/bin/sh
# HiveMind client launcher — resolves the Chromium sandbox before starting
# Electron, so a misconfigured sandbox fails with an actionable message
# instead of a silent abort (the packaged "won't open" freeze).
set -u
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
HELPER="$DIR/chrome-sandbox"
BINARY="$DIR/hivemind-client.bin"

sandbox_ready() {
  [ -x "$HELPER" ] || return 1
  [ -u "$HELPER" ] || return 1
  [ "$(stat -L -c %u "$HELPER" 2>/dev/null)" = "0" ] || return 1
  return 0
}

userns_available() {
  [ "$(cat /proc/sys/kernel/unprivileged_userns_clone 2>/dev/null)" = "1" ] || return 1
  [ "$(cat /proc/sys/user/max_user_namespaces 2>/dev/null)" -gt 0 ] 2>/dev/null || return 1
  # Ubuntu 23.10+ / distros with AppArmor-restricted user namespaces: Chromium
  # cannot use the namespace sandbox even though the userns sysctls look enabled.
  [ "$(cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns 2>/dev/null)" = "1" ] && return 1
  return 0
}

nosuid_mount() {
  # A root-owned setuid helper cannot elevate on a nosuid mount (e.g. some
  # /data or /projects volumes); Chromium would die with a cryptic abort.
  findmnt -rn -o OPTIONS --target "$HELPER" 2>/dev/null | grep -qw nosuid
}

MESSAGE="HiveMind cannot start its Chromium sandbox.
Repair the setuid sandbox helper once:
  sudo chown root:root \"$HELPER\"
  sudo chmod 4755 \"$HELPER\"
(or run ./install.sh in this directory)."

if sandbox_ready; then
  if nosuid_mount; then
    MESSAGE="HiveMind's Chromium sandbox helper is installed, but this filesystem is mounted 'nosuid', so it cannot gain the privileges Electron needs.
Move the HiveMind folder to a filesystem that allows setuid binaries (for example your home directory), then run ./install.sh there."
    printf '%s\n' "$MESSAGE" >&2
    if command -v zenity >/dev/null 2>&1; then
      zenity --error --title "HiveMind could not start" --text "$MESSAGE" >/dev/null 2>&1 &
    fi
    exit 1
  fi
  exec "$BINARY" "$@"
fi

if userns_available; then
  exec "$BINARY" --disable-setuid-sandbox "$@"
fi

printf '%s\n' "$MESSAGE" >&2
# Surface a GUI hint for desktop-file launches; backgrounded so the launcher
# still exits immediately instead of waiting for the dialog to be dismissed.
if command -v zenity >/dev/null 2>&1; then
  zenity --error --title "HiveMind could not start" --text "$MESSAGE" >/dev/null 2>&1 &
fi
exit 1

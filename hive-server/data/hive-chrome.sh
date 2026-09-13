#!/bin/bash
# hive-chrome.sh — launch (or focus) the shared-profile Chrome on the
# container desktop (:99). Single source of truth for the launch flags:
# used by hive-screen.sh at stack bring-up AND by the openbox desktop
# menu when the user relaunches the browser from noVNC.
#
# Behaviour:
#  - CDP up   → an instance already owns the profile; ask IT for a new
#               window (profile singleton forwards the request), so the
#               user and the agent always share ONE browser session.
#  - CDP down → clear stale Singleton locks (survive container recreates)
#               and launch fresh with CDP :9222 for browser_driver.py.
set -u
export DISPLAY="${DISPLAY:-:99}"

CHROME="$(command -v google-chrome \
    || ls /opt/ms-playwright/chromium-*/chrome-linux64/chrome \
          /opt/ms-playwright/chromium-*/chrome-linux/chrome 2>/dev/null \
    | tail -1)"
[ -z "$CHROME" ] && { echo "hive-chrome: no chrome found" >&2; exit 1; }
PROFILE=/home/hive/.browser-profile
URL="${1:-about:blank}"

if curl -sf --max-time 2 http://127.0.0.1:9222/json/version >/dev/null 2>&1; then
    # Live instance on this profile — singleton forwards a new-window
    # request to it. No CDP flags needed on this side.
    nohup "$CHROME" --user-data-dir="$PROFILE" "$URL" \
        >/dev/null 2>&1 &
    exit 0
fi

# Fresh launch. --no-sandbox: rootless podman can't run Chrome's SUID
# sandbox (the container is the sandbox boundary).
rm -f "$PROFILE"/Singleton{Lock,Socket,Cookie} 2>/dev/null || true
nohup "$CHROME" --remote-debugging-port=9222 \
    --user-data-dir="$PROFILE" --no-first-run \
    --no-default-browser-check --disable-session-crashed-bubble \
    --disable-dev-shm-usage --no-sandbox --disable-gpu --noerrdialogs \
    --hide-crash-restore-bubble \
    --disable-features=TranslateUI \
    --start-maximized "$URL" >/tmp/chromium.log 2>&1 &

#!/bin/bash
# hive-chrome.sh — launch (or focus) the shared-profile Chrome on the
# container desktop (:99). Single source of truth for the launch flags:
# used by hive-screen.sh at stack bring-up AND by the openbox desktop
# menu when the user relaunches the browser from noVNC.
#
# Behaviour:
#  - Phase 72.2: multi-agent bare-metal — CDP port/profile derive from
#    the per-user SLOT (same formula as browser_driver.py::_default_slot
#    and hive-screen.sh:_slot — change all three or none); the container
#    user "hive" and hivex-queenbee stay on the legacy 9222.
#  - CDP up   → an instance already owns the profile; ask IT for a new
#               window (profile singleton forwards the request), so the
#               user and the agent always share ONE browser session.
#  - CDP down → clear stale Singleton locks (survive container recreates)
#               and launch fresh with CDP :9222 for browser_driver.py.
set -u

_slot() {
    case "$(id -un)" in
        hivex-queenbee) echo 0;;
        hivex-*) id -un | tr -d '\n' | od -An -v -tu1 \
                 | awk '{for(i=1;i<=NF;i++)s+=$i} END{print s%50+1}';;
        *) echo 0;;
    esac
}
SLOT="${HIVE_BROWSER_SLOT:-$(_slot)}"
CDP="${HIVE_BROWSER_CDP_PORT:-$((9222 + SLOT))}"
export DISPLAY="${DISPLAY:-:$((99 + SLOT))}"

CHROME="$(command -v google-chrome \
    || ls /opt/ms-playwright/chromium-*/chrome-linux64/chrome \
          /opt/ms-playwright/chromium-*/chrome-linux/chrome 2>/dev/null \
    | tail -1)"
[ -z "$CHROME" ] && { echo "hive-chrome: no chrome found" >&2; exit 1; }
PROFILE="${HIVE_BROWSER_PROFILE:-$HOME/.browser-profile}"
URL="${1:-about:blank}"

if curl -sf --max-time 2 "http://127.0.0.1:$CDP/json/version" >/dev/null 2>&1; then
    # Live instance on this profile — singleton forwards a new-window
    # request to it. No CDP flags needed on this side.
    setsid -f nohup "$CHROME" --user-data-dir="$PROFILE" "$URL" \
        >/dev/null 2>&1 &
    exit 0
fi

# Fresh launch. --no-sandbox: rootless podman can't run Chrome's SUID
# sandbox (the container is the sandbox boundary).
rm -f "$PROFILE"/Singleton{Lock,Socket,Cookie} 2>/dev/null || true
setsid -f nohup "$CHROME" --remote-debugging-port="$CDP" \
    --user-data-dir="$PROFILE" --no-first-run \
    --no-default-browser-check --disable-session-crashed-bubble \
    --disable-dev-shm-usage --no-sandbox --disable-gpu --noerrdialogs \
    --hide-crash-restore-bubble \
    --disable-features=TranslateUI \
    --start-maximized \
    --renderer-process-limit=6 \
    --js-flags=--max-old-space-size=384 \
    "$URL" >/tmp/chromium.$SLOT.log 2>&1 &

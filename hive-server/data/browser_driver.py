#!/usr/bin/env python3
"""browser_driver.py — HiveMind Phase 6 computer-use driver.

Runs INSIDE the agent container. Connects to a persistent Chromium over CDP
(launching it on the GNOME Wayland headless display when needed), so the same
browser the agent drives is the one the user sees in the web UI's Computer
view — that's the login-handoff channel: the user can watch, take over, log
in, and the agent resumes on the same page. Profile persists at
~/.browser-profile.

Phase 24: the browser is playwright's bundled chromium (Ubuntu's
chromium-browser deb is a snap stub — unusable in containers) running
Wayland-native on gnome-shell's headless display.

Usage: python3 browser_driver.py <action> [args...]
Prints one JSON object to stdout: {"status": "ok", ...} or
{"status": "error", "error": "..."}. Secret VALUES come in via environment
variables only (HIVE_SECRET_<NAME>) and are never echoed back.
"""
import json
import os
import subprocess
import sys
import time

CDP_URL = "http://127.0.0.1:9222"
PROFILE = os.path.expanduser("~/.browser-profile")
TEXT_LIMIT = 6000  # chars returned to the agent — protect the LLM context


def emit(obj):
    print(json.dumps(obj))
    sys.exit(0 if obj.get("status") == "ok" else 1)


def chromium_binary():
    """Playwright's bundled chromium (the snap stub can't run here)."""
    import glob
    hits = sorted(glob.glob(
        "/opt/ms-playwright/chromium-*/chrome-linux64/chrome") +
        glob.glob("/opt/ms-playwright/chromium-*/chrome-linux/chrome") +
        glob.glob(os.path.expanduser(
            "~/.cache/ms-playwright/chromium-*/chrome-linux*64/chrome")) +
        glob.glob(os.path.expanduser(
            "~/.cache/ms-playwright/chromium-*/chrome-linux/chrome")))
    if not hits:
        # Real Chrome first (login handoff: sites reject testing builds),
        # then whatever chromium is around.
        for c in ("google-chrome", "chromium", "chromium-browser"):
            if subprocess.run(["which", c], capture_output=True).returncode == 0:
                return c
        raise RuntimeError("no chromium found (run: playwright install chromium)")
    return hits[-1]  # newest


def ensure_display():
    """Bring up the Xvfb screen stack if :99 has no X server.

    The worker runs screens on demand (hub screen_ctl op when the user
    opens Computer view, torn down after idle) — so :99 usually is NOT up
    when a browser tool fires. hive-screen.sh is idempotent: it spawns
    Xvfb/Xvnc/websockify then watchdogs them. Detach it so the watchdog
    outlives this driver invocation.
    """
    if os.path.exists("/tmp/.X11-unix/X99"):
        return
    subprocess.Popen(
        ["hive-screen.sh", os.environ.get("HIVE_SCREEN_RES", "1280x800")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True)
    for _ in range(40):  # up to ~20s for Xvfb to create its socket
        if os.path.exists("/tmp/.X11-unix/X99"):
            return
        time.sleep(0.5)


def ensure_browser(pw):
    """Connect to the running Chromium; launch it if CDP is unreachable."""
    try:
        return pw.chromium.connect_over_cdp(CDP_URL)
    except Exception:
        pass
    ensure_display()
    os.makedirs(PROFILE, exist_ok=True)
    env = {**os.environ,
           # X11 stack (Phase 25): the Xvfb display the user sees via noVNC.
           "DISPLAY": os.environ.get("DISPLAY", ":99")}
    subprocess.Popen(
        [chromium_binary(), "--remote-debugging-port=9222",
         f"--user-data-dir={PROFILE}", "--no-first-run", "--no-default-browser-check",
         "--disable-session-crashed-bubble", "--disable-dev-shm-usage",
         # Rootless podman can't run Chrome's SUID/namespace sandbox — the
         # browser process dies on launch without this (the agent container
         # itself is the sandbox boundary).
         "--no-sandbox",
         "--disable-gpu", "--noerrdialogs",
         "--disable-features=TranslateUI", "--start-maximized", "about:blank"],
        env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    last = None
    for _ in range(30):  # up to ~15s for first launch
        try:
            return pw.chromium.connect_over_cdp(CDP_URL)
        except Exception as e:  # still starting
            last = e
            time.sleep(0.5)
    emit({"status": "error", "error": f"chromium did not come up: {last}"})


def current_page(browser):
    for ctx in browser.contexts:
        if ctx.pages:
            return ctx.pages[-1]
    return browser.contexts[0].new_page() if browser.contexts else None


def main():
    if len(sys.argv) < 2:
        emit({"status": "error", "error": "usage: browser_driver.py <action> [args...]"})
    action, args = sys.argv[1], sys.argv[2:]

    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        emit({"status": "error",
              "error": "python3-playwright not installed in this container"})

    with sync_playwright() as pw:
        browser = ensure_browser(pw)
        page = current_page(browser)
        if page is None:
            emit({"status": "error", "error": "no browser context"})
        try:
            if action == "goto":
                page.goto(args[0], wait_until="domcontentloaded", timeout=45000)
                emit({"status": "ok", "url": page.url, "title": page.title()})
            elif action == "click":
                page.click(args[0], timeout=10000)
                emit({"status": "ok", "url": page.url})
            elif action == "type":
                page.fill(args[0], args[1], timeout=10000)
                emit({"status": "ok"})
            elif action == "press":
                page.keyboard.press(args[0])
                emit({"status": "ok"})
            elif action == "text":
                body = page.inner_text("body")
                emit({"status": "ok", "url": page.url, "title": page.title(),
                      "text": body[:TEXT_LIMIT],
                      "truncated": len(body) > TEXT_LIMIT})
            elif action == "screenshot":
                path = args[0] if args else "/workspace/screenshot.png"
                page.screenshot(path=path, full_page=False)
                emit({"status": "ok", "path": path})
            elif action == "eval":
                result = page.evaluate(args[0])
                emit({"status": "ok",
                      "result": json.dumps(result)[:TEXT_LIMIT]})
            elif action == "back":
                page.go_back(wait_until="domcontentloaded", timeout=15000)
                emit({"status": "ok", "url": page.url})
            elif action == "reload":
                page.reload(wait_until="domcontentloaded", timeout=15000)
                emit({"status": "ok", "url": page.url})
            elif action == "tabs":
                emit({"status": "ok", "tabs": [
                    {"url": p.url, "title": p.title()}
                    for c in browser.contexts for p in c.pages]})
            elif action == "secret":
                # secret <selector> <ENV_NAME> — value from env, never echoed
                selector, env_name = args[0], args[1]
                value = os.environ.get(env_name)
                if value is None:
                    emit({"status": "error",
                          "error": f"secret env {env_name} not set "
                                   "(not granted to this agent)"})
                page.fill(selector, value, timeout=10000)
                emit({"status": "ok"})   # deliberately: no value, no echo
            else:
                emit({"status": "error", "error": f"unknown action '{action}'"})
        except Exception as e:
            # playwright errors are verbose — one line is plenty
            emit({"status": "error",
                  "error": str(e).splitlines()[0][:300]})


if __name__ == "__main__":
    main()

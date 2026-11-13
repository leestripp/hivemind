#!/usr/bin/env python3
"""Prove that leaving and re-entering voice mode never interrupts an agent.

Launches an isolated second Hive Dash against a fake hub, then drives the same
UiApi path as the voice page's Back button. The fake hub records every request,
so this checks the network contract rather than inferring it from visible text.

Requires a desktop display. Run with:
    cmake --build build --target check-voice-back-ui
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.request import Request, urlopen


AGENT_ID = "agent-back-test"
THREAD_ID = "thread-back-test"
AGENT = {
    "id": AGENT_ID,
    "name": "Back Test Agent",
    "role": "Navigation regression test",
    "avatar": "user",
    "state": "online",
    "online": True,
    "busy": True,
}
THREAD = {
    "id": THREAD_ID,
    "title": AGENT["name"],
    "kind": "dm",
    "agent_ids": [AGENT_ID],
    "last_activity": "2026-11-10T00:00:00Z",
}


class FakeHub:
    def __init__(self) -> None:
        self.requests: list[tuple[str, str]] = []
        self.lock = threading.Lock()
        test = self

        class Handler(BaseHTTPRequestHandler):
            def record(self) -> None:
                with test.lock:
                    test.requests.append((self.command, self.path))

            def reply(self, value: object, status: int = 200) -> None:
                payload = json.dumps(value).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def do_GET(self) -> None:  # noqa: N802 — stdlib handler API
                self.record()
                path = self.path.split("?", 1)[0]
                if path == "/healthz":
                    self.reply({"ok": True})
                elif path == "/api/agents":
                    self.reply({"agents": [AGENT]})
                elif path == "/api/agents/activity":
                    self.reply({"agents": {AGENT_ID: {"busy": True}}})
                elif path == "/api/threads":
                    self.reply({"threads": [THREAD]})
                elif path == "/api/workers":
                    self.reply({"workers": [{"name": "hub", "online": True}]})
                elif path == "/api/thread-groups":
                    self.reply({"groups": []})
                elif path == f"/api/threads/{THREAD_ID}/messages":
                    self.reply({"messages": []})
                elif path == f"/api/threads/{THREAD_ID}/activity":
                    self.reply({"busy": True, "cancelling": False,
                                "partial": "Still working", "error": ""})
                else:
                    self.reply({"error": f"unexpected GET {self.path}"}, 404)

            def do_POST(self) -> None:  # noqa: N802 — stdlib handler API
                self.record()
                length = int(self.headers.get("Content-Length", "0"))
                if length:
                    self.rfile.read(length)
                path = self.path.split("?", 1)[0]
                if path == "/api/threads":
                    self.reply(THREAD)
                elif path == f"/api/threads/{THREAD_ID}/read":
                    self.reply({"ok": True})
                else:
                    # Cancellation attempts are intentionally successful: the
                    # request log below, not a fake-hub failure, is the assertion.
                    self.reply({"ok": True, "cancelled": 1})

            def log_message(self, _format: str, *_args: object) -> None:
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self) -> "FakeHub":
        self.thread.start()
        return self

    def __exit__(self, *_args: object) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.server.server_port}"

    def interruptions(self) -> list[tuple[str, str]]:
        with self.lock:
            return [(method, path) for method, path in self.requests
                    if method == "POST" and
                    path.split("?", 1)[0].endswith(("/cancel", "/stop"))]


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def call(base: str, method: str, path: str, body: object | None = None) -> dict:
    data = None if body is None else json.dumps(body).encode()
    request = Request(base + path, data=data, method=method,
                      headers={"Content-Type": "application/json"})
    with urlopen(request, timeout=10) as response:
        return json.load(response)


def wait_for(predicate, description: str, timeout: float = 20.0) -> None:
    deadline = time.monotonic() + timeout
    latest = None
    while time.monotonic() < deadline:
        latest = predicate()
        if latest:
            return
        time.sleep(0.1)
    raise AssertionError(f"timed out waiting for {description}; latest={latest}")


def main() -> None:
    client = (Path(__file__).resolve().parents[2] /
              "build" / "hive-client" / "hive-client")
    if not client.is_file():
        raise SystemExit(f"build hive-client first: {client} not found")
    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        raise SystemExit("this UI regression requires DISPLAY or WAYLAND_DISPLAY")

    with FakeHub() as hub, tempfile.TemporaryDirectory(prefix="hive-back-test-") as home:
        home_path = Path(home)
        config = home_path / ".config" / "hivemind"
        config.mkdir(parents=True)
        (config / "config.json").write_text(json.dumps({
            "server": hub.url, "api_key": "back-test-key",
            "display_name": "Back Test",
        }))
        ui_port = free_port()
        ui = f"http://127.0.0.1:{ui_port}"
        env = os.environ.copy()
        env.update({
            "HOME": home,
            "CHIMPFLOW_URL": hub.url,
            "CHIMPFLOW_KEY": "back-test-key",
            "HIVEMIND_MULTI_INSTANCE": "1",
            "HIVEMIND_UI_PORT": str(ui_port),
        })
        log = (home_path / "client.log").open("w")
        process = subprocess.Popen([str(client)], env=env,
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_for(lambda: _healthy(ui), "Hive Dash UiApi")
            wait_for(lambda: any(a.get("id") == AGENT_ID for a in
                                 call(ui, "GET", "/ui/state").get("agents", [])),
                     "fake hub roster")
            for attempt in range(2):
                selected = call(ui, "POST", "/ui/action/select-agent",
                                {"agent": AGENT_ID})
                assert selected.get("selected") == AGENT_ID, selected
                wait_for(lambda: call(ui, "GET", "/ui/state").get("voice_mode"),
                         f"voice mode entry {attempt + 1}")
                before = call(ui, "GET", "/ui/state")
                result = call(ui, "POST", "/ui/action/voice-back")
                after = call(ui, "GET", "/ui/state")
                assert result.get("was_open") is True, result
                assert after.get("voice_mode") is False, after
                assert after.get("current_thread") == before.get("current_thread"), (
                    before, after)
                time.sleep(1)  # let panel teardown and queued I/O finish
                assert not hub.interruptions(), hub.interruptions()

            state = call(ui, "GET", "/ui/state")
            assert state.get("sidebar_visible") is True, state
            assert state.get("chat_visible") is True, state
            assert not hub.interruptions(), hub.interruptions()
            print("PASS: Back → roster → same agent → Back preserved the DM and "
                  "sent no agent/thread cancel or stop request")
        finally:
            try:
                call(ui, "POST", "/ui/action/close")
                process.wait(timeout=10)
            except (OSError, subprocess.TimeoutExpired):
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            log.close()
            if process.returncode not in (0, -15):
                print((home_path / "client.log").read_text())


def _healthy(ui: str) -> bool:
    try:
        return call(ui, "GET", "/ui/health").get("ok") is True
    except OSError:
        return False


if __name__ == "__main__":
    main()

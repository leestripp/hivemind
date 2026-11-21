#!/usr/bin/env python3
"""End-to-end GTK messaging regression against an isolated fake hub.

Covers the 2026-11-20/21 DM outage: enveloped POST /api/threads responses,
wrong-DM rejection, real composer editing/clicking, per-thread drafts,
failed-send preservation/retry, and attachment ownership. No production hub,
agent, ChimpFlow service, or user configuration is touched.
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
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import gi
gi.require_version("Atspi", "2.0")
from gi.repository import Atspi

AGENTS = [
    {"id": "agent-other", "name": "Other", "role": "test", "avatar": "user",
     "state": "ready", "online": True, "worker": "hub"},
    {"id": "agent-target", "name": "HoneyBee", "role": "test", "avatar": "user",
     "state": "ready", "online": True, "worker": "hub"},
]
THREADS = [
    {"id": "thread-other", "title": "Other", "kind": "dm",
     "agent_ids": ["agent-other"], "last_activity": "2026-11-20T00:00:00Z"},
    {"id": "thread-target", "title": "HoneyBee", "kind": "dm",
     "agent_ids": ["agent-target"], "last_activity": "2026-11-20T00:00:01Z"},
]


class FakeHub:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.message_posts: list[tuple[str, dict]] = []
        self.fail_messages = False
        self.invalid_ack = False
        self.open_mode = "envelope"
        self.open_delays: dict[str, float] = {}
        outer = self

        class Handler(BaseHTTPRequestHandler):
            def reply(self, value: object, status: int = 200) -> None:
                payload = json.dumps(value).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def do_GET(self) -> None:  # noqa: N802
                path = self.path.split("?", 1)[0]
                if path == "/healthz":
                    self.reply({"ok": True})
                elif path == "/api/agents":
                    self.reply({"agents": AGENTS})
                elif path == "/api/agents/activity":
                    self.reply({"agents": {a["id"]: {"busy": False} for a in AGENTS}})
                elif path == "/api/threads":
                    self.reply({"threads": THREADS})
                elif path == "/api/workers":
                    self.reply({"workers": [{"name": "hub", "online": True}]})
                elif path == "/api/thread-groups":
                    self.reply({"groups": []})
                elif path.endswith("/messages"):
                    self.reply({"messages": []})
                elif path.endswith("/activity"):
                    self.reply({"busy": False, "cancelling": False,
                                "partial": "", "error": ""})
                else:
                    self.reply({"error": f"unexpected GET {path}"}, 404)

            def do_POST(self) -> None:  # noqa: N802
                length = int(self.headers.get("Content-Length", "0"))
                raw = self.rfile.read(length) if length else b"{}"
                path = self.path.split("?", 1)[0]
                try:
                    body = json.loads(raw)
                except (json.JSONDecodeError, UnicodeDecodeError):
                    body = {}
                if path == "/api/threads":
                    requested = body.get("agent_ids", [])
                    if requested:
                        time.sleep(outer.open_delays.get(requested[0], 0.0))
                    if outer.open_mode == "envelope":
                        self.reply({"threads": THREADS})
                    elif outer.open_mode == "direct":
                        match = next(t for t in THREADS if t["agent_ids"] == requested)
                        self.reply(match, 201)
                    else:  # requested agent absent: client must not use a decoy
                        self.reply({"threads": [THREADS[1]]})
                elif path.endswith("/messages"):
                    with outer.lock:
                        outer.message_posts.append((path, body))
                        fail = outer.fail_messages
                    if fail:
                        self.reply({"error": "injected send failure"}, 503)
                    elif outer.invalid_ack:
                        self.reply({"status": "processing"}, 202)
                    else:
                        self.reply({"message_id": "message-test",
                                    "status": "processing"}, 202)
                elif path.endswith("/attachments"):
                    self.reply({"name": "brief.txt", "path": "attachments/brief.txt",
                                "host_path": "/fake/brief.txt", "size": len(raw)}, 201)
                elif path.endswith("/read"):
                    self.reply({"ok": True})
                else:
                    self.reply({"error": f"unexpected POST {path}"}, 404)

            def log_message(self, *_args: object) -> None:
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def __enter__(self) -> "FakeHub":
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()

    def close(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.server.server_port}"


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def call(base: str, method: str, path: str, body: object | None = None) -> dict:
    data = None if body is None else json.dumps(body).encode()
    request = Request(base + path, data=data, method=method,
                      headers={"Content-Type": "application/json"})
    try:
        with urlopen(request, timeout=10) as response:
            return json.load(response)
    except HTTPError as error:
        return json.load(error)


def wait_for(predicate, description: str, timeout: float = 20.0):
    deadline = time.monotonic() + timeout
    latest = None
    while time.monotonic() < deadline:
        try:
            latest = predicate()
            if latest:
                return latest
        except Exception as error:  # AT-SPI registry and app startup race
            latest = repr(error)
        time.sleep(0.1)
    raise AssertionError(f"timed out waiting for {description}; latest={latest}")


def descendants(root, depth: int = 0):
    if depth > 28:
        return []
    result = [root]
    for index in range(root.get_child_count()):
        result.extend(descendants(root.get_child_at_index(index), depth + 1))
    return result


def client_app():
    desktop = Atspi.get_desktop(0)
    for index in range(desktop.get_child_count()):
        app = desktop.get_child_at_index(index)
        if app.get_name() == "hive-client":
            return app
    return None


def composer_entry():
    app = client_app()
    if not app:
        return None
    for obj in descendants(app):
        if obj.get_role_name() == "text" and obj.is_editable_text():
            states = obj.get_state_set()
            if states.contains(Atspi.StateType.SHOWING) and \
                    states.contains(Atspi.StateType.SENSITIVE):
                return obj
    return None


def send_button():
    app = client_app()
    for obj in descendants(app):
        if obj.get_role_name() == "button" and obj.get_name() == "Send message":
            return obj
    raise AssertionError("real Send message button not found")


def set_entry(text: str) -> None:
    entry = wait_for(composer_entry, "sensitive real composer")
    assert entry.set_text_contents(text), "AT-SPI rejected editable-text replacement"


def click_send() -> None:
    button = send_button()
    button.do_action(0)  # the actual GTK clicked signal, not a synthetic HTTP send


def state(ui: str) -> dict:
    return call(ui, "GET", "/ui/state")


def select_agent(ui: str, agent: str) -> None:
    result = call(ui, "POST", "/ui/action/select-agent", {"agent": agent})
    assert result.get("selected") == agent, result


def select_thread(ui: str, thread: str) -> None:
    result = call(ui, "POST", "/ui/action/select-thread", {"id": thread})
    assert result.get("selected") == thread, result


def main() -> None:
    root = Path(__file__).resolve().parents[2]
    binary = Path(os.environ.get("HIVEMIND_CLIENT_BINARY", "")) if \
        os.environ.get("HIVEMIND_CLIENT_BINARY") else root / "build-devpc/hive-client/hive-client"
    if not binary.is_file():
        binary = root / "build/hive-client/hive-client"
    if not binary.is_file():
        raise SystemExit("build hive-client first")
    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        raise SystemExit("this GTK regression requires DISPLAY or WAYLAND_DISPLAY")

    with FakeHub() as hub, tempfile.TemporaryDirectory(prefix="hive-messaging-test-") as home:
        home_path = Path(home)
        config = home_path / ".config/hivemind"
        config.mkdir(parents=True)
        (config / "config.json").write_text(json.dumps({
            "server": hub.url, "api_key": "test-key", "display_name": "Test User"}))
        ui = f"http://127.0.0.1:{free_port()}"
        env = os.environ.copy()
        env.update({"HOME": home, "CHIMPFLOW_URL": hub.url, "CHIMPFLOW_KEY": "",
                    "HIVEMIND_MULTI_INSTANCE": "1",
                    "HIVEMIND_UI_PORT": ui.rsplit(":", 1)[1]})
        with (home_path / "client.log").open("w") as log:
            process = subprocess.Popen([str(binary)], env=env,
                                       stdout=log, stderr=subprocess.STDOUT)
            try:
                wait_for(lambda: call(ui, "GET", "/ui/health").get("ok"), "UiApi")
                wait_for(lambda: len(state(ui).get("agents", [])) == 2, "roster")
                wait_for(client_app, "AT-SPI application")

                # Enveloped response with a decoy first: select the exact DM.
                select_agent(ui, "agent-target")
                wait_for(lambda: state(ui).get("current_thread") == "thread-target",
                         "HoneyBee DM")
                wait_for(lambda: state(ui).get("entry_sensitive"), "enabled composer")
                assert state(ui)["current_dm_agent"] == "agent-target"

                # Real editable text + real Send button, with long pasted Unicode.
                instructions = "Update SeeOzTogether — " + ("API instructions μ " * 180)
                set_entry(instructions)
                wait_for(lambda: state(ui).get("entry_text") == instructions,
                         "real composer contents")
                click_send()
                wait_for(lambda: len(hub.message_posts) == 1, "first message POST")
                path, body = hub.message_posts[-1]
                assert path == "/api/threads/thread-target/messages", (path, body)
                assert body == {"body": instructions}, body
                wait_for(lambda: state(ui).get("entry_text") == "", "accepted draft clear")
                wait_for(lambda: state(ui).get("entry_sensitive"), "composer re-enabled")

                # Drafts belong to their conversation, not whichever is visible next.
                set_entry("HoneyBee unsent draft")
                select_thread(ui, "thread-other")
                wait_for(lambda: state(ui).get("entry_text") == "", "other empty draft")
                set_entry("Other unsent draft")
                select_thread(ui, "thread-target")
                wait_for(lambda: state(ui).get("entry_text") == "HoneyBee unsent draft",
                         "HoneyBee draft restoration")
                select_thread(ui, "thread-other")
                wait_for(lambda: state(ui).get("entry_text") == "Other unsent draft",
                         "Other draft restoration")

                # A failed POST never clears or misroutes the draft; retry succeeds.
                select_thread(ui, "thread-target")
                wait_for(lambda: state(ui).get("entry_text") == "HoneyBee unsent draft",
                         "target draft before failed send")
                hub.fail_messages = True
                click_send()
                wait_for(lambda: "draft preserved" in state(ui).get("status", ""),
                         "failed-send status")
                assert state(ui)["entry_text"] == "HoneyBee unsent draft"
                assert state(ui)["entry_sensitive"] is True
                hub.fail_messages = False
                click_send()
                wait_for(lambda: hub.message_posts[-1][1].get("body") ==
                         "HoneyBee unsent draft", "failed-draft retry")
                assert hub.message_posts[-1][0] == "/api/threads/thread-target/messages"
                wait_for(lambda: state(ui).get("entry_text") == "", "retry draft clear")

                # A malformed 2xx acknowledgement is not success either.
                set_entry("Draft surviving an invalid acknowledgement")
                hub.invalid_ack = True
                click_send()
                wait_for(lambda: "draft preserved" in state(ui).get("status", ""),
                         "invalid-acknowledgement draft preservation")
                assert state(ui)["entry_text"] == \
                    "Draft surviving an invalid acknowledgement"
                hub.invalid_ack = False
                click_send()
                wait_for(lambda: state(ui).get("entry_text") == "",
                         "valid-acknowledgement retry")

                # Attachment remains with its originating thread across switches.
                upload = home_path / "brief.txt"
                upload.write_text("attachment regression")
                call(ui, "POST", "/ui/action/attach", {"path": str(upload)})
                wait_for(lambda: state(ui).get("pending_attachments") == 1,
                         "attachment ready")
                select_thread(ui, "thread-other")
                wait_for(lambda: state(ui).get("pending_attachments") == 0,
                         "other thread has no target attachment")
                select_thread(ui, "thread-target")
                wait_for(lambda: state(ui).get("pending_attachments") == 1,
                         "target attachment restored")
                set_entry("Attachment message")
                click_send()
                wait_for(lambda: hub.message_posts[-1][1].get("attachments"),
                         "attachment POST")
                assert hub.message_posts[-1][0] == "/api/threads/thread-target/messages"

                # Missing requested membership is an error, NEVER a decoy fallback.
                hub.open_mode = "missing-target"
                select_agent(ui, "agent-target")  # same active DM: no network needed
                select_agent(ui, "agent-other")
                wait_for(lambda: "Could not open Other" in state(ui).get("status", ""),
                         "safe open failure")
                assert state(ui)["current_thread"] == "thread-target"
                assert state(ui)["entry_sensitive"] is True

                # Canonical direct responses work too. Delayed stale opens must
                # never override the latest agent selection or unlock sending.
                hub.open_mode = "direct"
                select_agent(ui, "agent-other")
                wait_for(lambda: state(ui).get("current_thread") == "thread-other",
                         "direct-response DM")
                hub.open_delays = {"agent-target": 0.8, "agent-other": 0.05}
                select_agent(ui, "agent-target")
                wait_for(lambda: state(ui).get("opening_dm_agent") == "agent-target",
                         "delayed DM open")
                assert state(ui)["send_sensitive"] is False
                rejected = call(ui, "POST", "/ui/action/send", {"body": "too early"})
                assert "error" in rejected, rejected
                select_agent(ui, "agent-other")
                wait_for(lambda: state(ui).get("current_thread") == "thread-other" and
                         not state(ui).get("opening_dm_agent"), "latest DM wins")
                time.sleep(1.0)
                assert state(ui)["current_thread"] == "thread-other"
                assert all(body.get("body") != "too early"
                           for _path, body in hub.message_posts)

                print("PASS: real GTK composer, exact-DM selection, direct/enveloped "
                      "responses, long Unicode send, per-thread drafts/attachments, "
                      "failed-send preservation/retry, and wrong-DM rejection")
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


if __name__ == "__main__":
    main()

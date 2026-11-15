#!/usr/bin/env python3
"""Exercise master volume/mute and GSettings persistence in real Hive Dash."""

import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
BINARY = ROOT / "build/hive-client/hive-client"
SCHEMA_DIR = ROOT / "build/hive-client/schemas"


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def request(port, path, payload=None):
    data = None if payload is None else json.dumps(payload).encode()
    req = urllib.request.Request(
        f"http://127.0.0.1:{port}{path}", data=data,
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as response:
        return json.load(response)


def wait_for(port, predicate, description):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        state = request(port, "/ui/state")
        if predicate(state):
            return state
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {description}: {state}")


def launch(home, run_name):
    port = free_port()
    env = os.environ.copy()
    env["HOME"] = home
    env["XDG_CONFIG_HOME"] = f"{home}/.config"
    env["XDG_CACHE_HOME"] = f"{home}/.cache"
    env["XDG_DATA_HOME"] = f"{home}/.local/share"
    env["HIVEMIND_UI_PORT"] = str(port)
    env["GSETTINGS_SCHEMA_DIR"] = str(SCHEMA_DIR)
    log_path = Path(home) / f"{run_name}.log"
    with log_path.open("w") as log:
        process = subprocess.Popen(
            [str(BINARY)], env=env, stdout=log, stderr=subprocess.STDOUT)
    deadline = time.monotonic() + 15
    while True:
        if process.poll() is not None:
            raise AssertionError(
                f"client exited {process.returncode}:\n{log_path.read_text()}")
        try:
            request(port, "/ui/health")
            return process, port, log_path
        except Exception:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.1)


def close(process, port, log_path):
    request(port, "/ui/action/close", {})
    process.wait(timeout=10)
    assert process.returncode == 0, process.returncode
    log_text = log_path.read_text()
    assert "Gtk-WARNING" not in log_text, log_text
    assert "Gtk-CRITICAL" not in log_text, log_text


def set_volume(port, volume, muted):
    return request(port, "/ui/action/master-volume",
                   {"volume": volume, "muted": muted})


def main():
    if "--inside-bus" not in sys.argv:
        # D-Bus activated services inherit the DAEMON'S environment. Isolate
        # HOME before creating the private daemon so dconf and both app runs
        # share one temporary persistent database, never the user's database.
        with tempfile.TemporaryDirectory(prefix="hive-volume-ui-") as home:
            env = os.environ.copy()
            env["HOME"] = home
            env["XDG_CONFIG_HOME"] = f"{home}/.config"
            env["XDG_CACHE_HOME"] = f"{home}/.cache"
            env["XDG_DATA_HOME"] = f"{home}/.local/share"
            env["GSETTINGS_SCHEMA_DIR"] = str(SCHEMA_DIR)
            env["HIVE_VOLUME_TEST_HOME"] = home
            completed = subprocess.run([
                "dbus-run-session", "--", sys.executable, __file__,
                "--inside-bus"], env=env, check=False)
            return completed.returncode

    home = os.environ["HIVE_VOLUME_TEST_HOME"]
    process = None
    try:
        process, port, log_path = launch(home, "first-run")
        state = request(port, "/ui/state")
        assert state["master_volume"] == 1.0, state
        assert state["master_muted"] is False, state
        assert state["master_gain"] == 1.0, state

        # Activate the real speaker button callback and verify its popover.
        opened = request(port, "/ui/action/open-dialog", {"name": "volume"})
        assert opened == {"opened": "volume"}, opened
        wait_for(port, lambda s: s["volume_popover_visible"],
                 "speaker popover")

        # Set the actual Gtk::Scale and Gtk::CheckButton, exercising their
        # normal value-changed/toggled callbacks.
        result = set_volume(port, 0.35, False)
        assert abs(result["gain"] - 0.35) < 0.0001, result
        result = set_volume(port, 0.35, True)
        assert result["gain"] == 0.0, result
        assert abs(result["volume"] - 0.35) < 0.0001, result

        # Slider changes while muted remain silent; unmute restores gain.
        set_volume(port, 0.65, True)
        result = set_volume(port, 0.65, False)
        assert abs(result["gain"] - 0.65) < 0.0001, result
        assert set_volume(port, 0.0, False)["gain"] == 0.0

        # Leave a non-default value for the full-process restart check.
        result = set_volume(port, 0.42, False)
        assert abs(result["gain"] - 0.42) < 0.0001, result
        close(process, port, log_path)
        process = None

        process, port, log_path = launch(home, "second-run")
        state = request(port, "/ui/state")
        assert abs(state["master_volume"] - 0.42) < 0.0001, state
        assert abs(state["master_gain"] - 0.42) < 0.0001, state
        assert state["master_muted"] is False, state

        set_volume(port, 1.0, False)
        close(process, port, log_path)
        process = None
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

    print("PASS: master volume/mute UI and GSettings restore after relaunch")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Live bubble narration Play/Stop regression against a running hive-client.

Requires GNOME AT-SPI, the client's loopback UiApi, a visible agent message,
and ChimpFlow. Never captures the screen; it activates accessible buttons and
reads the UiApi playback ledger. Set HIVEMIND_UI_URL to override the default.
"""

import json
import os
import time
import urllib.request

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402

BASE = os.environ.get("HIVEMIND_UI_URL", "http://127.0.0.1:8481").rstrip("/")


def state():
    with urllib.request.urlopen(BASE + "/ui/state", timeout=10) as response:
        return json.load(response)


def walk(obj, depth=0):
    if depth > 30:
        return
    yield obj, depth
    for index in range(obj.get_child_count()):
        yield from walk(obj.get_child_at_index(index), depth + 1)


def app():
    desktop = Atspi.get_desktop(0)
    for index in range(desktop.get_child_count()):
        candidate = desktop.get_child_at_index(index)
        if candidate.get_name() == "hive-client":
            return candidate
    raise AssertionError("hive-client is not visible through AT-SPI")


def speech_buttons(prefix):
    return [
        obj
        for obj, _ in walk(app())
        if obj.get_role_name() == "button"
        and (obj.get_name() or "").startswith(prefix)
    ]


def wait_for(predicate, timeout=10):
    deadline = time.monotonic() + timeout
    latest = None
    while time.monotonic() < deadline:
        latest = state()
        if predicate(latest):
            return latest
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for playback state: {latest}")


def main():
    plays = speech_buttons("Read ")
    assert plays, "no readable agent bubble in the current thread"
    plays[-1].do_action(0)
    wait_for(lambda value: value["chat_tts_button_active"])
    stops = speech_buttons("Stop reading ")
    assert len(stops) == 1, f"expected one Stop button, got {len(stops)}"

    stops[0].do_action(0)
    wait_for(
        lambda value: value["chat_tts_outstanding"] == 0
        and not value["chat_tts_speaking"]
        and not value["chat_tts_button_active"]
    )
    # A stale delayed synthesis/audio callback must not resurrect the state.
    time.sleep(0.4)
    latest = state()
    assert latest["chat_tts_outstanding"] == 0, latest
    assert not latest["chat_tts_speaking"], latest
    assert not latest["chat_tts_button_active"], latest
    assert not speech_buttons("Stop reading ")
    print("PASS: bubble Play -> Stop -> silent idle -> Play; no stale revival")


if __name__ == "__main__":
    main()

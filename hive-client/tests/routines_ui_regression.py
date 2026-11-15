#!/usr/bin/python3
"""Live, read-only Scheduled Tasks UI regression check.

Run against a running hive-client with its UiApi enabled:
    /usr/bin/python3 hive-client/tests/routines_ui_regression.py

Requires GNOME AT-SPI (gi.repository.Atspi). The only destructive-looking
action is clicking a task's ✕ to verify the confirmation appears; the test
always chooses Cancel and verifies the task count never changes.
"""
import json
import re
import time
import urllib.request

import gi
gi.require_version("Atspi", "2.0")
from gi.repository import Atspi

BASE = "http://127.0.0.1:8481"


def post_dialog():
    request = urllib.request.Request(
        BASE + "/ui/action/open-dialog", data=b'{"name":"tasks"}',
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(request, timeout=10) as response:
        assert json.load(response).get("opened") == "tasks"


def descendants(root, depth=0):
    if depth > 26:
        return []
    result = [root]
    for index in range(root.get_child_count()):
        result.extend(descendants(root.get_child_at_index(index), depth + 1))
    return result


def client_dialog(title):
    desktop = Atspi.get_desktop(0)
    for index in range(desktop.get_child_count()):
        app = desktop.get_child_at_index(index)
        if app.get_name() != "hive-client":
            continue
        for obj in descendants(app):
            if obj.get_role_name() in ("frame", "dialog", "alert") and obj.get_name() == title:
                return obj
    raise AssertionError(f"dialog not found: {title}")


def buttons(root, label):
    return [obj for obj in descendants(root)
            if obj.get_role_name() == "button" and obj.get_name() == label]


def selected(root):
    return [obj for obj in descendants(root)
            if obj.get_state_set().contains(Atspi.StateType.SELECTED)]


def bounds(root):
    rect = root.get_component().get_extents(Atspi.CoordType.SCREEN)
    return rect.width, rect.height


def main():
    post_dialog()
    time.sleep(4)
    dialog = client_dialog("Scheduled Tasks")
    objects = descendants(dialog)
    status = next(obj.get_name() for obj in objects
                  if re.fullmatch(r"\d+ tasks across \d+ agents",
                                  obj.get_name() or ""))
    match = re.fullmatch(r"(\d+) tasks across (\d+) agents", status)
    task_count, group_count = map(int, match.groups())
    groups = []
    for obj in objects:
        if obj.get_role_name() != "button":
            continue
        group = re.match(r"^.+?  \((\d+)\)", obj.get_name() or "")
        if group:
            groups.append((obj, int(group.group(1))))
    assert len(groups) == group_count, (len(groups), group_count)
    assert len(buttons(dialog, "Edit")) == task_count
    assert len(buttons(dialog, "▶")) == task_count
    assert len(buttons(dialog, "✕")) == task_count
    width, height = bounds(dialog)
    assert 0 < width <= 900 and 0 < height <= 900, (width, height)
    for label in ("Edit", "▶", "✕"):
        assert all(all(value > 0 for value in bounds(button))
                   for button in buttons(dialog, label))
    assert not selected(dialog), "task group unexpectedly selected"

    group, count = groups[0]
    group.do_action(0)  # collapse
    time.sleep(1)
    assert len(buttons(dialog, "Edit")) == task_count - count
    assert not selected(dialog), "collapsing selected the group"
    group.do_action(0)  # expand again
    time.sleep(1)
    assert len(buttons(dialog, "Edit")) == task_count
    assert not selected(dialog), "expanding selected the group"

    buttons(dialog, "✕")[0].do_action(0)
    time.sleep(1)
    confirmation = client_dialog("Delete scheduled task?")
    assert buttons(confirmation, "Cancel")
    assert buttons(confirmation, "Delete")
    assert len(buttons(dialog, "Edit")) == task_count
    buttons(confirmation, "Cancel")[0].do_action(0)
    time.sleep(1)
    assert len(buttons(dialog, "Edit")) == task_count
    print(f"PASS: {task_count} tasks / {group_count} groups; bounded {width}x{height}; "
          "actions visible; no selection; collapse/expand; delete cancelled safely")


if __name__ == "__main__":
    main()

#!/bin/bash
# blender-mcp-up.sh — idempotent launcher for Blender's MCP socket server.
# Baked into the :gpu image; run by hive-agent-init.sh at container start
# (and safe for agents to re-run any time).
#
# Chain: hive-brain spawns the `blender-mcp` stdio bridge (mcp_servers in the
# spawn frame) -> bridge connects to this TCP server on localhost:9876 ->
# requests execute inside Blender via the official lab/blender_mcp addon.
#
# Background-mode notes (verified 2026-09-07 on Blender 5.1.2):
# - The addon registers the `blender_mcp` CLI command in register(), so the
#   extension must be ENABLED (persisted in userpref) before the server can
#   start. Enable+save is done here once per container volume.
# - Background mode is synchronous-only: deferred responses are rejected
#   (interactive/timer mode needs the UI event loop on the Xvfb desktop).
# - Code results come back via the `result` variable, not return/print.
set -u

PORT="${BLENDER_MCP_PORT:-9876}"
LOG=/tmp/blender-mcp-server.log

port_up() { python3 -c "import socket,sys; sys.exit(0 if socket.socket().connect_ex(('127.0.0.1', $PORT))==0 else 1)"; }

if port_up; then
    exit 0
fi

PREF_DIR="$HOME/.config/blender"
# Enable the extension once per volume (persists in userpref.blend).
if [ ! -f "$PREF_DIR/.mcp-enabled" ]; then
    blender --online-mode --background --python-expr \
        "import bpy; bpy.ops.preferences.addon_enable(module='bl_ext.system.mcp'); bpy.ops.wm.save_userpref()" \
        >>"$LOG" 2>&1 || true
    mkdir -p "$PREF_DIR" && touch "$PREF_DIR/.mcp-enabled"
fi

PYTHONUNBUFFERED=1 nohup blender --online-mode --background \
    --command blender_mcp --port "$PORT" >>"$LOG" 2>&1 &

for _ in $(seq 1 30); do
    port_up && { echo "blender-mcp-up: server listening on :$PORT"; exit 0; }
    sleep 1
done
echo "blender-mcp-up: server did not come up — see $LOG" >&2
exit 1

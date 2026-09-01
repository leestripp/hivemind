# hive-server/data/blender-mcp-autostart.py — copied into Blender's
# scripts/startup/ by Containerfile.hive-agent-gpu. Trusted startup
# scripts always run (not gated by the auto-execute preference), UI or
# background. Enabling the addon runs its register(), which AUTO-STARTS
# the MCP socket server on localhost:9876 — the blender-mcp stdio bridge
# (spawned by daedalus) connects to it.
import bpy

try:
    bpy.ops.preferences.addon_enable(module="blender_mcp")
    print("[blender-mcp] addon enabled — MCP socket server auto-started on :9876")
except Exception as e:
    print("[blender-mcp] autostart: addon enable failed:", e)

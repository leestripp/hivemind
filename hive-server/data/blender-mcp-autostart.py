# hive-server/data/blender-mcp-autostart.py — copied into Blender's
# scripts/startup/ by Containerfile.hive-agent-gpu. Trusted startup
# scripts always run (not gated by the auto-execute preference).
#
# WHY THE TIMER: Blender 5.x runs startup scripts under _RestrictContext,
# and the MCP extension's register() touches bpy.context.view_layer —
# enabling inline here fails with "'_RestrictContext' object has no
# attribute 'view_layer'" (verified 5.0.1/5.1.x). Deferring via
# bpy.app.timers fires the enable inside the UI event loop with full
# context. The extension's autostart preference (default True) then
# brings up the TCP socket server on localhost:9876, which the
# blender-mcp stdio bridge (spawned by daedalus) connects to.
import bpy


def _enable_mcp_addon():
    for mod in ("bl_ext.system.mcp", "bl_ext.user_default.mcp"):
        try:
            bpy.ops.preferences.addon_enable(module=mod)
            print(f"[blender-mcp] autostart: enabled {mod} — socket :9876")
            return None
        except Exception:
            continue
    print("[blender-mcp] autostart: MCP extension not found in any repo")
    return None


bpy.app.timers.register(_enable_mcp_addon, first_interval=2.0)

#!/bin/bash
# hive-agent-init.sh — :gpu container CMD. Starts auxiliary services the
# agent's tools depend on, then idles (hive-worker execs hive-agent in).
set -u
if command -v blender-mcp >/dev/null 2>&1; then
    blender-mcp-up.sh || true
fi
exec sleep infinity

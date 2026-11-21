#!/bin/sh
# =============================================================================
# HiveMind desktop client — launcher for the persistent cpp-dev container.
#
# Runs the installed ~/.local/bin/hive-client with the container's existing GPU, Wayland,
# PipeWire/Pulse audio and D-Bus session integration. Client settings persist
# in cpp-dev's /home/daedalus. The container must already be running; launching
# the client does not stop or replace the development container.
#
# Optional overrides:
#   HIVEMIND_CLIENT_CONTAINER  container name (default: cpp-dev)
#   HIVEMIND_CLIENT_BINARY     client path inside the container
#
# Install: cp hive-client-container.sh ~/.local/bin/hive-client-container
# =============================================================================
set -eu

CONTAINER="${HIVEMIND_CLIENT_CONTAINER:-cpp-dev}"
CLIENT="${HIVEMIND_CLIENT_BINARY:-/home/daedalus/.local/bin/hive-client}"

if ! podman container exists "$CONTAINER"; then
    echo "hive-client: container '$CONTAINER' does not exist" >&2
    exit 1
fi
if [ "$(podman inspect --format '{{.State.Running}}' "$CONTAINER")" != "true" ]; then
    echo "hive-client: container '$CONTAINER' is not running; start it first" >&2
    exit 1
fi
if ! podman exec "$CONTAINER" test -x "$CLIENT"; then
    echo "hive-client: '$CLIENT' is not executable in $CONTAINER; deploy it first" >&2
    exit 1
fi

exec podman exec \
    --env HOME=/home/daedalus \
    --env XDG_RUNTIME_DIR=/tmp \
    --env WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}" \
    --env DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/bus \
    --env GDK_BACKEND=wayland \
    --env QT_QPA_PLATFORM=wayland \
    --workdir /home/daedalus \
    "$CONTAINER" "$CLIENT" "$@"

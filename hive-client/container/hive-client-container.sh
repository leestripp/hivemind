#!/bin/sh
# =============================================================================
# HiveMind desktop client — rootless podman wrapper (host-side launcher).
#
# Passthrough: GPU (/dev/dri), Wayland, PipeWire/Pulse audio sockets and the
# host D-Bus session bus via /run/user/1000 (distrobox-style); host /home/lee
# bind-mounted rw so settings/data persist on the host. --userns keep-id maps
# the container user to host lee, so files written in the container are owned
# by lee on the host.
#
# Install: cp hive-client-container.sh ~/.local/bin/hive-client-container
# =============================================================================
set -eu

IMAGE="localhost/hive-client:latest"

exec podman run --rm --replace \
    --name hive-client \
    --userns keep-id \
    --network host \
    --device /dev/dri \
    -e HOME=/home/lee \
    -e WAYLAND_DISPLAY \
    -e XDG_RUNTIME_DIR=/run/user/1000 \
    -e DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
    -e GDK_BACKEND=wayland \
    -e QT_QPA_PLATFORM=wayland \
    -v /run/user/1000:/run/user/1000 \
    -v /home/lee:/home/lee \
    -w /home/lee \
    "$IMAGE" "$@"

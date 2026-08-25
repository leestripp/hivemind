#!/bin/sh
# install.sh — hive-server on a fresh Ubuntu 26.04+ machine.
# Run as root:  sh install.sh /path/to/HiveMind
#
# What it does:
#   1. creates the hivemind system user (rootless podman home)
#   2. installs systemd units (server + daily backup timer)
#   3. installs /usr/local/bin/hive-deploy + hive-backup
#   4. enables and starts hive-server + the backup timer
#
# The server runs from a git checkout at ~hivemind/hivemind (build from
# source on the target — the binary is not relocatable across distros).
# Prereqs: podman, sqlite3, cmake, g++, git, rsync (for backups).
set -e

SRC="${1:?usage: install.sh /path/to/HiveMind-checkout}"
[ -f "$SRC/hive-server/deploy/hive-server.service" ] || {
    echo "error: $SRC does not look like a HiveMind checkout"; exit 1; }

# 1. user
if ! id hivemind >/dev/null 2>&1; then
    useradd -m -s /bin/bash hivemind
    loginctl enable-linger hivemind   # rootless podman survives logout
fi

# 2. checkout: copy (or link) the source into ~hivemind
if [ ! -d /home/hivemind/hivemind ]; then
    cp -a "$SRC" /home/hivemind/hivemind
    chown -R hivemind:hivemind /home/hivemind/hivemind
fi

# 3. units + scripts
install -m 644 "$SRC/hive-server/deploy/hive-server.service" /etc/systemd/system/
install -m 644 "$SRC/hive-server/deploy/hive-backup.service" /etc/systemd/system/
install -m 644 "$SRC/hive-server/deploy/hive-backup.timer"   /etc/systemd/system/
install -m 755 "$SRC/hive-server/deploy/hive-deploy" /usr/local/bin/
install -m 755 "$SRC/hive-server/deploy/hive-backup" /usr/local/bin/
systemctl daemon-reload

# 4. build as the service user, then start
runuser -u hivemind -- sh -c "cd /home/hivemind/hivemind && \
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j$(nproc) --target hive-server"
systemctl enable --now hive-server
systemctl enable --now hive-backup.timer

sleep 2
systemctl is-active hive-server && curl -s http://127.0.0.1:8421/api/health
echo
echo "hive-server installed. Next: create an API key (see docs/deploy.md §3)."

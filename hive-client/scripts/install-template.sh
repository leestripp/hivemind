#!/bin/sh
# Install the HiveMind sandbox helper and desktop entry for the current user.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
HELPER="$DIR/chrome-sandbox"
LAUNCHER="$DIR/hivemind-client"

echo "Installing the Chromium sandbox helper (requires sudo)..."
sudo chown root:root "$HELPER"
sudo chmod 4755 "$HELPER"

DESKTOP_DIR="$HOME/.local/share/applications"
mkdir -p "$DESKTOP_DIR"
sed "s|^Exec=.*|Exec=$LAUNCHER|" "$DIR/org.hivemind.Client.desktop" > "$DESKTOP_DIR/org.hivemind.Client.desktop"
echo "Installed desktop entry: $DESKTOP_DIR/org.hivemind.Client.desktop"
echo "HiveMind is ready to launch."

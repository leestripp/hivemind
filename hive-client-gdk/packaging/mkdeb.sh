#!/bin/bash
# Build a .deb package for the HiveMind GTK4 client (Ubuntu 26.04).
#
#   hive-client/packaging/mkdeb.sh
#
# Output: build/hivemind-client_<ver>_amd64.deb
# Deps are computed from the actual binary via dpkg-shlibdeps.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build-deb"   # own dir — never clobber the main build config
PKG="hivemind-client"

# 0.5.0-8-g2388fba → 0.5.0-8  (strip git hash; deb revision allows one dash)
VER="$(git -C "$ROOT" describe --tags 2>/dev/null | sed 's/^v//; s/-g[0-9a-f]*$//')"
[ -n "$VER" ] || VER="0.0.0-1"

# ── build (client only, Release) ────────────────────────────────────
cmake -S "$ROOT" -B "$BUILD" -DHIVEMIND_BUILD_SERVER=OFF \
      -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$BUILD" -j"$(nproc)" --target hive-client

BIN="$BUILD/hive-client/hive-client"
[ -x "$BIN" ] || { echo "build failed: $BIN missing"; exit 1; }

# ── stage ───────────────────────────────────────────────────────────
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/DEBIAN" \
         "$STAGE/usr/bin" \
         "$STAGE/usr/share/applications" \
         "$STAGE/usr/share/metainfo" \
         "$STAGE/usr/share/icons/hicolor/scalable/apps"

install -m755 "$BIN" "$STAGE/usr/bin/hive-client"
install -m644 "$ROOT/hive-client/packaging/org.hivemind.Client.desktop" \
              "$STAGE/usr/share/applications/"
install -m644 "$ROOT/hive-client/packaging/org.hivemind.Client.metainfo.xml" \
              "$STAGE/usr/share/metainfo/"
install -m644 "$ROOT/hive-client/data/icons/hive.svg" \
              "$STAGE/usr/share/icons/hicolor/scalable/apps/org.hivemind.Client.svg"

# ── control (deps from the binary's actual linkage) ─────────────────
# dpkg-shlibdeps refuses to run without a debian/control in the CWD —
# give it a minimal one (separate from the final DEBIAN/control).
mkdir -p "$STAGE/debian"
printf 'Source: %s\nPackage: %s\nVersion: %s\n' "$PKG" "$PKG" "$VER" \
    > "$STAGE/debian/control"
DEPS="$(cd "$STAGE" && dpkg-shlibdeps -O -e"usr/bin/hive-client" \
        | sed 's/^shlibs:Depends=//')"
[ -n "$DEPS" ] || DEPS="libgtkmm-4.0-0, libgtk-4-1"

cat > "$STAGE/DEBIAN/control" <<CTRL
Package: $PKG
Version: $VER
Architecture: amd64
Maintainer: HiveMind <leestripp@users.noreply.gitlab.com>
Depends: $DEPS
Section: devel
Priority: optional
Homepage: https://gitlab.com/leestripp/hivemind
Description: HiveMind client — chat with your AI agent hive
 GTK4 desktop client for hive-server: agent roster, threads,
 approvals, remote screen share. Connects to any hive-server
 over HTTP(S) with an API key.
CTRL

OUT="$BUILD/${PKG}_${VER}_amd64.deb"
dpkg-deb --build --root-owner-group "$STAGE" "$OUT" > /dev/null
echo "built: $OUT"
dpkg-deb --info "$OUT" | grep -E "Package|Version|Depends"

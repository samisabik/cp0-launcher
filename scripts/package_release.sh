#!/bin/sh
# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
# SPDX-License-Identifier: MIT
#
# Packs the cross-built launcher into cp0-launcher-arm64.tar.gz for installing
# on a stock CardputerZero with install.sh. Build projects/APPLaunch first.
#
# Usage: scripts/package_release.sh [output-dir]   (default: <repo>/release)

set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${1:-$REPO/release}
DIST=$REPO/projects/APPLaunch/dist
STRIP=${STRIP:-aarch64-unknown-linux-gnu-strip}
command -v "$STRIP" >/dev/null 2>&1 || STRIP=aarch64-linux-gnu-strip

if [ ! -f "$DIST/M5CardputerZero-APPLaunch" ]; then
    echo "no build in $DIST: cross-build projects/APPLaunch first" >&2
    exit 1
fi

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
PKG=$STAGE/cp0-launcher
mkdir -p "$PKG"
"$STRIP" -o "$PKG/M5CardputerZero-APPLaunch" "$DIST/M5CardputerZero-APPLaunch"
cp -R "$DIST/APPLaunch/share" "$PKG/share"
install -m 755 "$REPO/scripts/device_install.sh" "$PKG/install.sh"
cp "$REPO/LICENSE" "$PKG/LICENSE"

mkdir -p "$OUT"
tar -C "$STAGE" -czf "$OUT/cp0-launcher-arm64.tar.gz" cp0-launcher
echo "$OUT/cp0-launcher-arm64.tar.gz ($(du -h "$OUT/cp0-launcher-arm64.tar.gz" | cut -f1))"

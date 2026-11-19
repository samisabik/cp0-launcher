#!/bin/sh
# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
# SPDX-License-Identifier: MIT
#
# Builds the cross-compile sysroot from a running CardputerZero over SSH.
# Development packages are fetched with `apt-get download` at the device's own
# versions and unpacked in a temporary folder (nothing is installed), and the
# device's runtime libraries are copied as they are, so the sysroot matches
# what the launcher links against on the device.
#
# CI has no device, so `--docker` builds the same sysroot from a Debian trixie
# arm64 container instead, installing the packages there.
#
# Usage: scripts/make_sysroot.sh [ssh-host | --docker] [output-dir]
#   ssh-host    default: cp0
#   output-dir  default: <repo>/sysroot

set -eu

HOST=${1:-cp0}
REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${2:-$REPO/sysroot}

# Headers the APPLaunch cross build includes or links against.
PACKAGES="libc6-dev linux-libc-dev libcrypt-dev libglib2.0-dev libdbus-1-dev libinput-dev
libxkbcommon-dev libudev-dev libfreetype-dev libpng-dev zlib1g-dev"

STAGE="$OUT.tmp"
rm -rf "$STAGE"
mkdir -p "$STAGE"

if [ "$HOST" = "--docker" ]; then
    echo "building sysroot in a debian:trixie arm64 container"
    docker run --rm -i --platform linux/arm64 debian:trixie sh -s -- $PACKAGES <<'CONTAINER' | tar -C "$STAGE" -xf -
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >&2
apt-get install -y -qq --no-install-recommends "$@" >&2
tar -C / -cf - usr/include usr/lib/aarch64-linux-gnu usr/lib/ld-linux-aarch64.so.1
CONTAINER
    INFO_CMD='echo "host=docker debian:trixie"'
else
    INFO_CMD=""
    echo "fetching development packages from $HOST"
    # shellcheck disable=SC2087
    ssh "$HOST" sh -s -- $PACKAGES <<'REMOTE' | tar -C "$STAGE" -xf -
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
# The -dev packages these headers depend on, at the versions apt would install.
apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts \
    --no-breaks --no-replaces --no-enhances "$@" 2>/dev/null |
    grep -E '^[a-z0-9]' | sed 's/:.*//' | grep -E -- '-dev$' | sort -u > list
apt-get download $(cat list) >/dev/null 2>&1
mkdir root
for deb in *.deb; do dpkg-deb -x "$deb" root; done
tar -C root -cf - usr
REMOTE

    echo "copying runtime libraries from $HOST"
    ssh "$HOST" 'tar -C / -cf - usr/lib/aarch64-linux-gnu usr/lib/ld-linux-aarch64.so.1 2>/dev/null' |
        tar -C "$STAGE" -xf -
fi

ln -s usr/lib "$STAGE/lib"

# Debian's .so links and linker scripts sometimes use absolute paths; on the
# build host those would point outside the sysroot.
python3 - "$STAGE" <<'PY'
import os
import sys

root = sys.argv[1]
for base, dirs, files in os.walk(root):
    for name in dirs + files:
        path = os.path.join(base, name)
        if not os.path.islink(path):
            continue
        target = os.readlink(path)
        if target.startswith("/"):
            os.unlink(path)
            os.symlink(os.path.relpath(os.path.join(root, target.lstrip("/")), base), path)
PY

if [ -n "$INFO_CMD" ]; then
    sh -c "$INFO_CMD" > "$STAGE/SYSROOT_INFO"
else
    ssh "$HOST" 'echo "host=$(hostname)"; . /etc/os-release; echo "os=$PRETTY_NAME"; echo "glibc=$(ldd --version | head -1)"' > "$STAGE/SYSROOT_INFO"
fi
echo "date=$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$STAGE/SYSROOT_INFO"

rm -rf "$OUT"
mv "$STAGE" "$OUT"
echo "sysroot ready: $OUT ($(du -sh "$OUT" | cut -f1))"

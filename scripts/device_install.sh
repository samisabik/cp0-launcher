#!/bin/sh
# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
# SPDX-License-Identifier: MIT
#
# Installs this launcher over the stock CardputerZero launcher. Shipped as
# install.sh in the release tarball; run it on the device from the unpacked
# folder as the normal user (it calls sudo where needed).
#
# The stock binary is kept as M5CardputerZero-APPLaunch.stock for rollback.

set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=/usr/share/APPLaunch
BIN=$ROOT/bin/M5CardputerZero-APPLaunch

if [ ! -x "$HERE/M5CardputerZero-APPLaunch" ] || [ ! -d "$HERE/share" ]; then
    echo "run install.sh from the unpacked release folder" >&2
    exit 1
fi
if [ ! -x "$BIN" ]; then
    echo "$BIN not found: this expects a CardputerZero with the stock launcher" >&2
    exit 1
fi

# Keep apt and the stock updaters from putting the stock launcher back.
sudo apt-mark hold applaunch >/dev/null
for unit in applaunch-updater.service applaunch-apt-update.service applaunch-apps-updater.service; do
    sudo systemctl mask --now "$unit" >/dev/null 2>&1 || true
done

# The launcher is a user service with no seat session, so NetworkManager
# needs this rule before it lets the netdev group scan and join Wi-Fi.
if [ -f "$HERE/50-cp0-wifi-scan.rules" ]; then
    sudo install -D -m 644 "$HERE/50-cp0-wifi-scan.rules" /etc/polkit-1/rules.d/50-cp0-wifi-scan.rules
fi

[ -e "$BIN.stock" ] || sudo cp -p "$BIN" "$BIN.stock"
sudo install -m 755 "$HERE/M5CardputerZero-APPLaunch" "$BIN"
sudo cp -R "$HERE/share/." "$ROOT/share/"

systemctl --user restart APPLaunch.service
echo "installed; the stock launcher is kept as $BIN.stock"

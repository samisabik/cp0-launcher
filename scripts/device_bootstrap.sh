#!/bin/sh
# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
# SPDX-License-Identifier: MIT
#
# Installs this launcher on a CardputerZero that has no stock launcher, such
# as DietPi with the M5Stack drivers. Shipped as bootstrap.sh in the release
# tarball; run it on the device from the unpacked folder as the user the
# launcher runs as (it calls sudo where needed). Safe to run again to update.
#
# The system needs logind, dbus with a user session, NetworkManager, polkit
# and PipeWire with pipewire-pulse first; see docs/dietpi-migration.md.

set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=/usr/share/APPLaunch
BIN=$ROOT/bin/M5CardputerZero-APPLaunch

if [ ! -x "$HERE/M5CardputerZero-APPLaunch" ] || [ ! -d "$HERE/share" ] || [ ! -f "$HERE/APPLaunch.service" ] ||
    [ ! -f "$HERE/cp0-fbcon-release.service" ] || [ ! -f "$HERE/99-cp0-backlight.rules" ]; then
    echo "run bootstrap.sh from the unpacked release folder" >&2
    exit 1
fi
if [ "$(id -u)" -eq 0 ]; then
    echo "run bootstrap.sh as the user the launcher runs as, not root" >&2
    exit 1
fi
if dpkg-query -W -f '${Status}' applaunch 2>/dev/null | grep -q "install ok installed"; then
    echo "the stock launcher package is installed: use install.sh instead" >&2
    exit 1
fi

sudo install -d -m 755 "$ROOT/bin" "$ROOT/share" "$ROOT/applications"
sudo install -m 755 "$HERE/M5CardputerZero-APPLaunch" "$BIN"
sudo cp -R "$HERE/share/." "$ROOT/share/"

# The launcher is a user service with no seat session, so NetworkManager and
# logind need these rules before they accept its Wi-Fi and power requests.
for rule in 50-cp0-wifi-scan.rules 50-cp0-power.rules; do
    sudo install -D -m 644 "$HERE/$rule" "/etc/polkit-1/rules.d/$rule"
done

sudo install -D -m 644 "$HERE/99-cp0-backlight.rules" /etc/udev/rules.d/99-cp0-backlight.rules
sudo udevadm control --reload
sudo udevadm trigger --action=add --subsystem-match=backlight

sudo install -D -m 644 "$HERE/APPLaunch.service" /etc/systemd/user/APPLaunch.service
sudo loginctl enable-linger "$(id -un)"

# The launcher draws on the LCD itself; a login prompt or the text console's
# blinking cursor there would draw over it.
sudo systemctl mask --now getty@tty1.service >/dev/null 2>&1
sudo install -D -m 644 "$HERE/cp0-fbcon-release.service" /etc/systemd/system/cp0-fbcon-release.service
sudo systemctl daemon-reload
sudo systemctl enable cp0-fbcon-release.service >/dev/null 2>&1
sudo systemctl restart cp0-fbcon-release.service

# An SSH server without PAM does not set this, and systemctl --user needs it.
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
systemctl --user daemon-reload
systemctl --user enable APPLaunch.service >/dev/null 2>&1
systemctl --user restart APPLaunch.service
echo "installed; APPLaunch.service starts at boot for $(id -un)"

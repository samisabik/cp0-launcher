#!/bin/sh
# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
# SPDX-License-Identifier: MIT
#
# Prepares a fresh DietPi (ARMv8 Raspberry Pi image) on a CardputerZero for
# this launcher: M5Stack drivers, boot config, the launcher's user, the
# session and network services it talks to, and the tuning described in
# docs/dietpi-migration.md. It does not install the launcher; run bootstrap.sh
# from the release tarball as the launcher's user afterwards.
#
# Run as root on the device, once DietPi's first-run setup has finished and
# the device is online, with this scripts/ folder copied over:
#
#   tar -C scripts -cf - . | ssh root@<device> \
#       'mkdir -p /root/cp0-provision && tar -C /root/cp0-provision -xf -'
#   ssh root@<device> sh /root/cp0-provision/dietpi_provision.sh
#
# Safe to run again. It ends by asking for a reboot: Wi-Fi moves from ifupdown
# to NetworkManager and the address can change, since the DHCP client differs.
#
# Settings, as environment variables:
#   CP0_USER       account the launcher runs as            (default: admin)
#   CP0_USB_WIFI   driver of the USB Wi-Fi adapter to keep unloaded until an
#                  app needs it, empty for none            (default: rtw88_8821au)
#   CP0_OPENSSH    1 replaces Dropbear with key-only OpenSSH when root has an
#                  authorized key                          (default: 1)
#   CP0_TAILSCALE  1 installs Tailscale, logged out        (default: 1)

set -eu

CP0_USER=${CP0_USER:-admin}
CP0_USB_WIFI=${CP0_USB_WIFI-rtw88_8821au}
CP0_OPENSSH=${CP0_OPENSSH:-1}
CP0_TAILSCALE=${CP0_TAILSCALE:-1}

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CONFIG=/boot/firmware/config.txt
CMDLINE=/boot/firmware/cmdline.txt
DRIVERS=/root/dtoverlays
export DEBIAN_FRONTEND=noninteractive

step() { printf '\n== %s\n' "$1"; }
die() { echo "error: $1" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run as root"
[ -f "$CONFIG" ] && [ -f "$CMDLINE" ] || die "$CONFIG or $CMDLINE not found: is this a Raspberry Pi image?"
[ -x /boot/dietpi/func/dietpi-set_hardware ] || die "this is not DietPi"
for file in cp0-usb-wifi cp0-panel-100m.dts 50-cp0-wifi-scan.rules 50-cp0-power.rules; do
    [ -f "$HERE/$file" ] || die "$HERE/$file is missing: copy the whole scripts/ folder"
done

step "packages"
apt-get update -qq
# Build tools stay installed: the drivers are rebuilt after a kernel upgrade.
apt-get install -y --no-install-recommends \
    build-essential device-tree-compiler git ca-certificates \
    sudo dbus dbus-user-session libpam-systemd polkitd network-manager rtkit \
    pipewire pipewire-pulse pipewire-alsa wireplumber alsa-utils pulseaudio-utils \
    rfkill iw iptables i2c-tools libinput10 libxkbcommon0 libfreetype6 ncurses-term \
    python3 curl ffmpeg micro hostapd dnsmasq rtl-sdr rtl-433 openssh-client \
    systemd-zram-generator
systemctl disable --now dnsmasq.service >/dev/null 2>&1 || true

step "drivers"
# Headers can be newer than the image's kernel, so both are installed together.
# Skipped once the modules exist: the kernel packages are held from then on.
if ! ls /lib/modules/*/extra/panel-mipi-dbi-m.ko >/dev/null 2>&1; then
    apt-get install -y --no-install-recommends linux-image-rpi-v8 linux-headers-rpi-v8
fi
# The newest kernel that has headers; it becomes the running one after reboot.
KVER=$(for dir in /lib/modules/*/build; do [ -e "$dir" ] && basename "$(dirname "$dir")"; done | sort -V | tail -1)
[ -n "$KVER" ] || die "no kernel headers under /lib/modules"
[ -d "$DRIVERS/.git" ] || git clone --depth=1 https://github.com/m5stack/m5stack-linux-dtoverlays.git "$DRIVERS"
if [ ! -f "/lib/modules/$KVER/extra/panel-mipi-dbi-m.ko" ]; then
    make -C "$DRIVERS/modules/CardputerZero" KERNELDIR="/lib/modules/$KVER/build" \
        EXTRADIR="/lib/modules/$KVER/extra" CONFIG_CARDPUTERO_V0_5=y install
fi
make -C "$DRIVERS/modules/CardputerZero" install_led_permissions
depmod -a "$KVER"
git -C "$DRIVERS" rev-parse HEAD > /etc/cardputerzero-dtoverlays.commit
systemctl disable fb_load.service >/dev/null 2>&1 || true
# This revision of the device has no camera.
rm -f /boot/firmware/overlays/camera-gpio16-high-overlay.dtbo /boot/firmware/overlays/camera-py12-high-overlay.dtbo
# The modules are built for one kernel; an upgrade would leave the screen dark.
apt-mark hold linux-image-rpi-v8 "linux-image-$KVER" linux-headers-rpi-v8 "linux-headers-$KVER" >/dev/null
echo "built for $KVER; kernel packages held"

step "boot config"
[ -f "$CONFIG.dietpi-orig" ] || cp "$CONFIG" "$CONFIG.dietpi-orig"
[ -f "$CMDLINE.dietpi-orig" ] || cp "$CMDLINE" "$CMDLINE.dietpi-orig"
if ! grep -q "^dtoverlay=cardputerzero-v5-overlay" "$CONFIG"; then
    cat >> "$CONFIG" <<'EOF'

#-------Cardputer Zero hardware-------
# Overlays and modules built from github.com/m5stack/m5stack-linux-dtoverlays,
# revision in /etc/cardputerzero-dtoverlays.commit
[all]
dtparam=i2c_arm=on
dtparam=i2s=on
dtparam=spi=on
dtoverlay=dwc2
dtoverlay=cardputerzero-v5-overlay
dtoverlay=bq27220_v5
dtoverlay=gpio-ir,gpio_pin=13,gpio_pull=up
dtoverlay=gpio-ir-tx,gpio_pin=12
EOF
fi
# A faster panel clock than the board overlay asks for; remove the line to go
# back if a panel shows glitches at this speed.
dtc -@ -I dts -O dtb -o /boot/firmware/overlays/cp0-panel-100m.dtbo "$HERE/cp0-panel-100m.dts"
grep -q "^dtoverlay=cp0-panel-100m" "$CONFIG" ||
    sed -i "/^dtoverlay=cardputerzero-v5-overlay$/a dtoverlay=cp0-panel-100m" "$CONFIG"
COUNTRY=$(sed -n 's/^AUTO_SETUP_NET_WIFI_COUNTRY_CODE=//p' /boot/dietpi.txt 2>/dev/null | head -1)
for arg in fbcon=map:1 fbcon=font:VGA8x8 ${COUNTRY:+cfg80211.ieee80211_regdom=$COUNTRY}; do
    grep -qF -- "$arg" "$CMDLINE" || sed -i "s/\$/ $arg/" "$CMDLINE"
done
echo i2c-dev > /etc/modules-load.d/cardputerzero.conf
ln -sf /dev/null "/etc/systemd/system/systemd-backlight@backlight:backlight.service"
echo "blacklist dvb_usb_rtl28xxu" > /etc/modprobe.d/blacklist-rtl-sdr.conf

step "user $CP0_USER"
id "$CP0_USER" >/dev/null 2>&1 || useradd -m -s /bin/bash "$CP0_USER"
usermod -aG audio,video,input,render,netdev,spi,i2c,gpio,dialout,plugdev,sudo,systemd-journal "$CP0_USER"
# The launcher runs privileged settings through `sudo -n`.
echo "$CP0_USER ALL=(ALL) NOPASSWD: ALL" > "/etc/sudoers.d/010-$CP0_USER-nopasswd"
chmod 440 "/etc/sudoers.d/010-$CP0_USER-nopasswd"
visudo -cf "/etc/sudoers.d/010-$CP0_USER-nopasswd" >/dev/null
USER_HOME=$(getent passwd "$CP0_USER" | cut -d: -f6)
if [ -s /root/.ssh/authorized_keys ] && [ ! -e "$USER_HOME/.ssh/authorized_keys" ]; then
    install -d -m 700 -o "$CP0_USER" -g "$CP0_USER" "$USER_HOME/.ssh"
    install -m 600 -o "$CP0_USER" -g "$CP0_USER" /root/.ssh/authorized_keys "$USER_HOME/.ssh/authorized_keys"
fi

step "session"
systemctl unmask systemd-logind.service dbus-org.freedesktop.login1.service >/dev/null 2>&1 || true
systemctl start dbus.socket systemd-logind.service
loginctl enable-linger "$CP0_USER"
for rule in 50-cp0-wifi-scan.rules 50-cp0-power.rules; do
    install -D -m 644 "$HERE/$rule" "/etc/polkit-1/rules.d/$rule"
done
# Enabled by the packages, used by nothing here. Disabling filter-chain also
# drops pipewire.socket, which the launcher's audio does need.
systemctl --global disable filter-chain.service mpris-proxy.service >/dev/null 2>&1 || true
systemctl --global enable pipewire.socket >/dev/null 2>&1 || true
install -d /etc/wireplumber/wireplumber.conf.d
cat > /etc/wireplumber/wireplumber.conf.d/50-cardputerzero-default-volume.conf <<'EOF'
# Default output volume for a sink WirePlumber has not seen before: 60% on
# the UI scale. The stock 40% is barely audible on the built-in speaker.
wireplumber.settings = {
  device.routes.default-sink-volume = 0.216
}
EOF

step "bluetooth"
/boot/dietpi/func/dietpi-set_hardware bluetooth enable >/dev/null

step "memory and power"
cat > /etc/systemd/zram-generator.conf <<'EOF'
[zram0]
zram-size = ram / 2
compression-algorithm = zstd
swap-priority = 100
EOF
cat > /etc/sysctl.d/99-cp0-zram.conf <<'EOF'
# Swap goes to compressed RAM first (zram, priority 100), so swapping early is
# cheap. Overrides vm.swappiness=1 from 97-dietpi.conf.
vm.swappiness=100
vm.page-cluster=0
EOF
cat > /etc/udev/rules.d/90-cp0-usb-autosuspend.rules <<'EOF'
# Let the internal USB hub and Ethernet chip suspend when idle. While either
# is awake the dwc2 controller takes thousands of interrupts per second.
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="05e3", ATTR{idProduct}=="0610", TEST=="power/control", ATTR{power/control}="auto"
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="0fe6", ATTR{idProduct}=="9900", TEST=="power/control", ATTR{power/control}="auto"
EOF

step "usb wi-fi"
install -m 755 "$HERE/cp0-usb-wifi" /usr/local/sbin/cp0-usb-wifi
if [ -n "$CP0_USB_WIFI" ]; then
    cat > /etc/modprobe.d/cp0-usb-wifi.conf <<EOF
# USB Wi-Fi drivers that load only through /usr/local/sbin/cp0-usb-wifi, so
# the onboard radio is always wlan0 and the USB bus sleeps while they are
# unused. One "blacklist <module>" line per driver.
blacklist $CP0_USB_WIFI
EOF
    cat > /etc/systemd/system/cp0-usb-wifi.service <<'EOF'
[Unit]
Description=Keep the USB Wi-Fi driver unloaded until an app asks for it

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/cp0-usb-wifi off

[Install]
WantedBy=multi-user.target
EOF
    systemctl daemon-reload
    systemctl enable cp0-usb-wifi.service >/dev/null 2>&1
fi

step "network"
# NetworkManager leaves an interface listed for ifupdown unmanaged, so the
# Wi-Fi credentials move to a keyfile and the ifupdown entry is set aside.
IFUPDOWN=/etc/network/interfaces.d/wlan0.conf
WPA=/etc/wpa_supplicant/wpa_supplicant.conf
if [ -f "$IFUPDOWN" ]; then
    SSID=$(sed -n 's/^[[:space:]]*ssid="\(.*\)"$/\1/p' "$WPA" | head -1)
    PSK=$(sed -n 's/^[[:space:]]*psk=//p' "$WPA" | head -1 | sed 's/^"\(.*\)"$/\1/')
    [ -n "$SSID" ] && [ -n "$PSK" ] || die "no Wi-Fi network in $WPA: connect with dietpi-config first"
    KEYFILE="/etc/NetworkManager/system-connections/$SSID.nmconnection"
    ( umask 077; cat > "$KEYFILE" <<EOF
[connection]
id=$SSID
type=wifi
interface-name=wlan0
autoconnect=true

[wifi]
mode=infrastructure
ssid=$SSID

[wifi-security]
key-mgmt=wpa-psk
psk=$PSK

[ipv4]
method=auto

[ipv6]
method=auto
EOF
    )
    mkdir -p /root/cp0-backup-ifupdown
    mv "$IFUPDOWN" /root/cp0-backup-ifupdown/wlan0.conf
    echo "Wi-Fi \"$SSID\" handed to NetworkManager; ifupdown entry kept in /root/cp0-backup-ifupdown"
fi
printf '[connection]\nwifi.powersave=2\n' > /etc/NetworkManager/conf.d/wifi-powersave.conf
systemctl disable NetworkManager-wait-online.service networking.service >/dev/null 2>&1 || true

if [ "$CP0_TAILSCALE" = 1 ] && ! command -v tailscale >/dev/null 2>&1; then
    step "tailscale"
    . /etc/os-release
    curl -fsSL -o /usr/share/keyrings/tailscale-archive-keyring.gpg \
        "https://pkgs.tailscale.com/stable/debian/$VERSION_CODENAME.noarmor.gpg"
    curl -fsSL -o /etc/apt/sources.list.d/tailscale.list \
        "https://pkgs.tailscale.com/stable/debian/$VERSION_CODENAME.tailscale-keyring.list"
    apt-get update -qq
    apt-get install -y --no-install-recommends tailscale
fi

if [ "$CP0_OPENSSH" = 1 ] && [ ! -x /usr/sbin/sshd ]; then
    step "openssh"
    if [ -s /root/.ssh/authorized_keys ]; then
        # Sorts before DietPi's dietpi.conf, which turns password logins on.
        install -d -m 755 /etc/ssh/sshd_config.d
        cat > /etc/ssh/sshd_config.d/10-key-only.conf <<'EOF'
PasswordAuthentication no
KbdInteractiveAuthentication no
PermitRootLogin prohibit-password
EOF
        /boot/dietpi/dietpi-software install 105 >/dev/null
        systemctl disable ssh.service >/dev/null 2>&1 || true
        systemctl enable ssh.socket >/dev/null 2>&1
        echo "OpenSSH installed, key-only; the host key changed"
    else
        echo "skipped: root has no authorized key, and key-only OpenSSH would lock you out"
    fi
fi

step "done"
cat <<EOF
Reboot now. The screen shows the boot log and then stays on the last line
until the launcher is installed.

Next, from a checkout of this repo on the build host:
  1. scripts/make_sysroot.sh <device> and cross-build (README)
  2. scripts/package_release.sh, copy the tarball over, unpack it and run
     ./bootstrap.sh as $CP0_USER
Not done here: whisper-cli (cross-built, see docs/dietpi-migration.md), the
time zone, and \`tailscale up\`.
EOF

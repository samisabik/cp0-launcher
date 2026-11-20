# Moving the cp0 to DietPi: agent guide

Goal: a CardputerZero that boots DietPi and runs this launcher with every
built-in app working as it does on the stock M5Stack image.

Facts under "Measured" were read from the working device on 2026-11-19. They
describe that device, not DietPi. Re-read them with the commands given before
relying on them.

## Rules

1. Keep the stock SD card untouched. It is the only known-good source of the
   M5Stack kernel modules. Image it first (`dd` to a file on the Mac) and do
   all DietPi work on a second card.
2. Work in the order of the phases. Each phase has a check; stop at the first
   check that fails and report it.
3. Phase 1 decides whether the move is possible. Do it before anything else.
4. Read values from the device or the repo. Do not copy version numbers,
   addresses or package lists out of this file into scripts.
5. Tailscale needs the user to re-authenticate. SSH keys and the Tailscale
   state are secrets: copy them only when the user asks.

## Measured on the stock image

Re-read with: `cat /etc/os-release /etc/rpi-issue`, `uname -r`,
`cat /proc/device-tree/model`, `/boot/firmware/config.txt`,
`/boot/firmware/cmdline.txt`, `lsmod`, `ls /lib/modules/$(uname -r)/extra`.

- Board: Raspberry Pi Compute Module 0, 512 MB. OS: Debian 13 (trixie), arm64,
  built with pi-gen stage4. Kernel: the Raspberry Pi `linux-image-rpi-v8`
  package.
- Hardware support comes from M5Stack's device-tree overlays and out-of-tree
  kernel modules. They are not in any Debian package (`dpkg -S` finds no
  owner). The revision is recorded in `/etc/cardputerzero-dtoverlays.commit`;
  the source is https://github.com/m5stack/m5stack-linux-dtoverlays.
  - Modules in `/lib/modules/<kernel>/extra/`, loaded today:
    `panel-mipi-dbi-m` (display), `pwm_bl_m5stack` (backlight),
    `tca8418_keypad_m5stack` (keyboard), `es8389_m5stack` (audio codec),
    `m5ioe1` (IO expander), `bq27xxx_battery` and `bq27xxx_battery_i2c`
    (fuel gauge).
  - Overlays named in `config.txt`: `cardputerzero-v5-overlay`, `bq27220_v5`,
    `gpio-ir` (pin 13), `gpio-ir-tx` (pin 12), `dwc2`, `vc4-kms-v3d`.
  - Display firmware: `/lib/firmware/cardputerzero,st7789v_lcd.bin`.
  - `cmdline.txt` carries `fbcon=map:1` and the Wi-Fi regulatory domain.
  - `/proc/cardputerzero_hw_id` exists and the launcher reads it.
- udev rules in `/etc/udev/rules.d/`: `99-cardputer-zero-lcd.rules` creates
  `/dev/fb_lcd`, which the launcher opens; `99-cardputer-zero-keyboard.rules`,
  `99-ignore-i2c-key.rules`, `90-cp0-usb-autosuspend.rules`.
- Module config: `/etc/modules-load.d/cardputerzero.conf`,
  `/etc/modprobe.d/cp0-blacklist.conf`, `/etc/modprobe.d/blacklist-rtl-sdr.conf`.
- The launcher runs as a systemd user service for `admin`
  (`/usr/lib/systemd/user/APPLaunch.service`, with a drop-in in
  `~/.config/systemd/user/APPLaunch.service.d/`). Lingering is on. It starts
  after `pipewire-pulse.service`. `getty@tty1` is masked.
- `admin` is in: `audio video input render netdev spi i2c gpio dialout plugdev`.
- System services the launcher talks to: NetworkManager, BlueZ, timedated,
  PipeWire with pipewire-pulse and WirePlumber, tailscaled.
- Launcher settings live in `~/.config/cardputerzero/config.json`.

## What the launcher needs at run time

Derive these from the repo, since they change with the code:

- Shared libraries: `ldd` on the built binary. Development packages are listed
  in `scripts/make_sysroot.sh`.
- External programs: `git grep -hoE 'tool_present\("[^"]+"\)' projects/APPLaunch/main`
  and `git grep -hoE '"/usr/(s?bin|local/bin)/[^"]+"' projects/APPLaunch/main ext_components/cp0_lvgl/src`.
  On the stock image `whisper-cli` is a hand-built binary in `/usr/local/bin`
  with no package; it has to be rebuilt from whisper.cpp.
- Device paths: `git grep -hoE '"/(dev|sys|proc)/[^"]+"' ext_components/cp0_lvgl/src/cp0`.
- Service unit and polkit rule: `scripts/APPLaunch.service` and
  `scripts/50-cp0-wifi-scan.rules`.
- `scripts/device_install.sh` replaces an existing stock launcher and stops if
  `/usr/share/APPLaunch/bin/M5CardputerZero-APPLaunch` is missing. A fresh
  DietPi has no such file; `scripts/device_bootstrap.sh` (`bootstrap.sh` in
  the tarball) installs there instead.

## Launcher build to install

State on 2026-11-20, after the cleanup merged in PRs #7 to #10. Re-read with
`git log --oneline -5 main` and `gh release list --limit 3`.

- Install a build of `main` at `c4717a8` or later. Release `launcher-v0.8.0`
  predates the cleanup. Either cross-build `main` as in the README, or have
  the user run the release workflow first.
- That build has not run on any device yet. Confirm it on the stock image
  before judging it on DietPi, so a launcher fault is not mistaken for a
  DietPi fault. On the stock card: install it, walk Settings through a few
  submenus, Apps and one confirm page such as reboot; open several apps and
  look for blank widgets or missing text; run one sudo-gated action such as a
  time change. Then repeat the same three checks on DietPi in phase 4.
- Sound goes through external programs only (`arecord`, `ffmpeg`, `pw-play`,
  `amixer`, and libpulse for volume). The launcher links no audio library and
  has no sound-card service of its own, so PipeWire with pipewire-pulse and
  alsa-utils are the whole audio dependency.
- The launcher runs as the user unit in `scripts/APPLaunch.service`. The
  `run_as_user` config key is ignored. If the launcher is ever started as
  root it runs external apps as the first account with a uid of 1000 or more,
  falling back to `pi`; on DietPi keep it a user unit so that path is unused.
- `.desktop` entries need `Name` and `Exec`; `Icon` is ignored, and the home
  grid picks the icon from the app name.
- Build profiles compile with `-O2` and a reduced LVGL widget and font set. A
  page that renders an empty box where the stock image showed a widget points
  at `projects/APPLaunch/*_config_defaults.mk`, not at DietPi.
- The cross-build compiles against `<repo>/sysroot`, made from the device by
  `scripts/make_sysroot.sh`. Once DietPi runs, rebuild the sysroot from the
  DietPi device and rebuild the launcher, since library versions may differ
  from the stock image.

## Found on DietPi

Phases 1 to 3 were done on 2026-11-20 on one device; the launcher boots to the
home screen with no login. Phase 4 (opening every app) is still open. Re-read
the state with `uname -r`, `cat /etc/cardputerzero-dtoverlays.commit`,
`tail -15 /boot/firmware/config.txt` and `cat /boot/firmware/cmdline.txt`.

Answers to what was unknown:

- The DietPi ARMv8 Raspberry Pi image boots the Compute Module 0.
- DietPi installs the same `linux-image-rpi-v8` series. `linux-headers-rpi-v8`
  can be newer than the kernel on the image; installing it pulls the matching
  kernel, so build the modules against the headers' version (pass `KERNELDIR`
  and `EXTRADIR`) and reboot into it.
- m5stack-linux-dtoverlays builds against that kernel with
  `make CONFIG_CARDPUTERO_V0_5=y install` in `modules/CardputerZero`. The
  modules are not DKMS: after a kernel upgrade, rebuild them or the screen and
  keyboard stay dead. The source is kept in `/root/dtoverlays` for that.
- `systemd-logind` is masked and dbus is absent, as expected. Unmasking
  logind and installing the packages in phase 2 is enough.

Things that differ from the stock image and are easy to miss:

- **Boot config.** The `config_setup` make target needs `/opt/rpi-config.py`
  and adds a camera overlay; the overlay lines were added by hand instead.
  `vc4-kms-v3d` is left out: the LCD driver does not need it. The LCD is then
  `fb1` behind `/dev/fb_lcd`, with the firmware framebuffer as `fb0`.
- **Panel clock.** `scripts/cp0-panel-100m.dts` raises the LCD's SPI clock
  above what the board overlay asks for, which roughly doubles the full-frame
  rate reaching the panel (compare `bytes_tx` in
  `/sys/class/spi_master/spi0/statistics/` over a few seconds of redraws). It
  must come after the board overlay in `config.txt`; removing the line
  restores the vendor clock. Checked by eye on one device.
- **Runtime directory.** The user's uid is not 1000 here. The launcher takes
  `XDG_RUNTIME_DIR` from its environment (`commount.c`); a hardcoded
  `/run/user/1000` made every page that keeps files there fail, which showed
  as "scan timed out" in Air.
- **Text console on the LCD.** With `fbcon=map:1` the boot log shows on the
  LCD, and the console stays bound to it afterwards, which shows as a blinking
  cursor through the launcher. `scripts/cp0-fbcon-release.service` unbinds it
  once boot finishes; `getty@tty1` is masked for the same reason.
- **Sysfs permissions.** The launcher writes the backlight and the 5 V output
  switches as a normal user. On DietPi both are writable by root only, so the
  screen stays lit in sleep and the brightness setting does nothing.
  `scripts/99-cp0-backlight.rules` gives the `video` group the backlight. The
  switches need M5Stack's `60-cardputerzero-leds.rules` and
  `cardputerzero-leds.conf`, which the `install_led_permissions` make target
  installs and plain `install` does not.
- **M5 splash.** The driver install enables `fb_load.service`. Disable it
  unless the splash is wanted.
- **USB Wi-Fi adapter.** The onboard chip and a USB adapter race for `wlan0`,
  and the launcher expects the onboard chip there, with `wlan1` for scanning
  and the portal. A USB adapter with its driver bound also keeps the USB bus
  awake, which costs the dwc2 controller thousands of interrupts per second
  (read the rate from `/proc/interrupts`). So the adapter's driver is
  blacklisted in `/etc/modprobe.d/cp0-usb-wifi.conf` and loaded on demand by
  `scripts/cp0-usb-wifi`, installed as `/usr/local/sbin/cp0-usb-wifi`: off at
  boot, on while Air is open or the portal's access point runs
  (`app_support::usb_wifi`, `portal_model.cpp`). The NetworkManager profile is
  bound to `wlan0` and the onboard MAC address.
- **NetworkManager.** DietPi configures Wi-Fi through ifupdown in
  `/etc/network/interfaces.d/`. NetworkManager leaves an interface listed
  there unmanaged, so move that file away after writing a keyfile with the
  same credentials. `dietpi-config` no longer reflects the network afterwards.
  The DHCP address changes because the client identifier does.
- **Bluetooth.** DietPi blacklists it; `dietpi-set_hardware bluetooth enable`
  lifts that and installs `pi-bluetooth`.
- **Dropbear.** DietPi's default SSH server has no PAM and no SFTP: sessions
  lack `XDG_RUNTIME_DIR` (so `systemctl --user` fails until it is exported)
  and `scp` and `scripts/make_sysroot.sh` uploads need plain `ssh` pipes. The
  device was moved to OpenSSH with `dietpi-software install 105`, which
  removes Dropbear. Key-only login is a drop-in in `/etc/ssh/sshd_config.d/`
  that sorts before DietPi's `dietpi.conf`.
- **Volume.** WirePlumber's default for a new sink is barely audible on the
  speaker. The stock image raises it with a WirePlumber setting
  (`device.routes.default-sink-volume`, see `stage2/05-cardputerzero/files`
  in CardputerZero/pi-gen); the same file is installed on the device.
  `rtkit` is not pulled in by PipeWire without recommends; install it.
- **RTL-SDR.** Debian's `rtl-sdr` package does not blacklist the DVB driver on
  this system; `/etc/modprobe.d/blacklist-rtl-sdr.conf` does.
- **IMU.** The bmi270 does not answer on I2C on this device and its probe
  fails at boot, so `bmi270_bmm150_overlay` is left out of `config.txt`. Not
  investigated.
- **Kernel upgrades.** The kernel and header packages are on `apt-mark hold`.
  To upgrade: unhold, upgrade, rebuild the modules in `/root/dtoverlays`
  against the new headers, reboot, hold again.
- **Memory and power.** Swap goes to zram first (`systemd-zram-generator`,
  with `vm.swappiness` raised in `/etc/sysctl.d/99-cp0-zram.conf`); DietPi's
  swap file stays as the fallback. The internal USB hub and Ethernet chip
  autosuspend through `/etc/udev/rules.d/90-cp0-usb-autosuspend.rules`. The
  `filter-chain` and `mpris-proxy` user units, `NetworkManager-wait-online`
  and the leftover ifupdown `networking.service` are disabled. Disabling
  `filter-chain` also removes `pipewire.socket`; enable that again. SSH is
  socket-activated. DietPi sets `temp_limit` in `config.txt` below the
  firmware default, which caps the CPU frequency early under load.
- **whisper.cpp.** `whisper-cli` cross-builds on the Mac with cmake against
  `<repo>/sysroot`. The Homebrew toolchain does not search the sysroot's
  `aarch64-linux-gnu` multiarch folders; pass them with `-B`, `-L` and
  `-isystem`.

## Phases

`scripts/dietpi_provision.sh` does phases 1 and 2 and the device setup under
"Found on DietPi" in one run on a fresh DietPi; its header has the usage and
settings. It has been run on the configured device, where it changes nothing,
but not yet on a fresh image, so read its output on first use. The phases
below describe what it does and how to check each one.

### 1. Boot and drivers (go or no-go)

1. Flash the DietPi ARMv8 Raspberry Pi image to the second card. Boot with a
   serial console (`enable_uart=1`) or over Ethernet/Wi-Fi preset in
   `dietpi.txt`, since the screen will be dark.
2. Record `uname -r` and the kernel package name. Install matching headers.
3. Build and install m5stack-linux-dtoverlays at the commit from the stock
   image. Install its overlays, the display firmware file, the udev rules and
   the module config files listed above.
4. Carry over the overlay and `dtparam` lines from the stock `config.txt` and
   the `fbcon` argument from `cmdline.txt`.

Check: `/dev/fb_lcd` exists and `cat /dev/urandom > /dev/fb_lcd` shows noise;
`evtest` on the keyboard device reports key presses; `aplay -l` lists the
codec; `/sys/class/power_supply` has the gauge;
`/sys/class/backlight/backlight/brightness` changes the backlight;
`/proc/cardputerzero_hw_id` is readable.

If the modules do not build or the board does not boot, stop. The fallback is
to slim the stock image instead (see the last section).

### 2. Session and services

1. Create user `admin` with the groups listed above, or change the unit and
   install scripts to the DietPi user. Pick one and apply it everywhere.
2. Unmask `systemd-logind`, install `dbus`, `dbus-user-session` and
   `libpam-systemd`, enable lingering for the user.
3. Install and enable NetworkManager, BlueZ, PipeWire, pipewire-pulse and
   WirePlumber. Move Wi-Fi off DietPi's default network setup to
   NetworkManager.
4. Install `scripts/50-cp0-wifi-scan.rules` into `/etc/polkit-1/rules.d/`.

Check: `loginctl show-user <user> -p Linger` says yes;
`systemctl --user status pipewire-pulse` is active after a reboot with no
login; `nmcli dev wifi list` works as the user without sudo;
`bluetoothctl show` lists the adapter.

### 3. Launcher

1. Cross-build and pack as in the README (`scripts/package_release.sh`).
2. Copy the tarball to the device, unpack it and run `./bootstrap.sh` as the
   launcher's user. It creates `/usr/share/APPLaunch`, installs the binary,
   `share/`, the polkit and udev rules and the user unit, masks `getty@tty1`
   and releases the text console from the LCD. Read
   `scripts/device_bootstrap.sh` for the exact steps.

Check: after a cold boot the home screen appears with no login; the top bar
shows battery and Wi-Fi; `systemctl --user show APPLaunch.service -p NRestarts`
stays at 0 for five minutes.

### 4. Apps

Install the external programs found in "What the launcher needs", plus
Tailscale from its own apt repository. Then open every app once.

Check, per type:
- system: Settings changes brightness and volume and they persist; Files
  lists the home directory; CLI and Python open; Power reboots.
- network: IP Panel shows addresses; Tailnet lists peers after login; Air
  scans; Portal starts and stops an AP on `wlan1`.
- radio: SDR, ADSB and Sat run with the dongle; Sensors, Spectrum, Keys and
  Fox run on the CC1101.
- hardware: IR learns and replays; Pins scans I2C.
- sound: Rec records and transcribes; Audio shows the mic; Tracker plays.
- Lock screen locks and unlocks. Low-battery and charging states display.

### 5. User data

Copy from the stock card when the user asks: `~/.config/cardputerzero`,
`~/.config/IR-Remote`, `~/Recordings`, `~/sat`, `~/spectrum`, shell
configuration. Run `tailscale up` with the user present.

## Effort

Estimate, not a measurement:

| Phase | Work | Time |
|---|---|---|
| 1 | Boot, kernel modules, overlays | 0.5 to 2 days; all the risk is here |
| 2 | logind, dbus, PipeWire, NetworkManager, BlueZ | 2 to 4 hours |
| 3 | Bootstrap script and unit | 1 to 2 hours |
| 4 | Tools, whisper.cpp rebuild, app-by-app checks | 3 to 5 hours |
| 5 | User data | under 1 hour |

## The cheaper alternative

Most of what DietPi removes (logind, dbus, NetworkManager, PipeWire, BlueZ) is
what the launcher needs, so the saving after phase 2 is smaller than a stock
DietPi suggests. The stock image is pi-gen stage4, which includes a desktop
the launcher never uses (`~/.config` still holds `labwc`, `wf-panel-pi` and
`kanshi` settings). Removing those packages from the working image keeps the
drivers and reaches a similar footprint with none of the phase 1 risk. Measure
first: `systemd-analyze blame`, `free -m`, and
`dpkg-query -W -f '${Installed-Size}\t${Package}\n' | sort -n | tail -40`.

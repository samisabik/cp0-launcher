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
  DietPi has no such file, so phase 3 needs a bootstrap path.

## Unknown, to settle in phase 1

- Whether a DietPi image boots a Compute Module 0. DietPi's hardware page
  lists the Zero 2 W for its ARMv8 image and does not name the CM0.
- Which kernel package DietPi's Raspberry Pi image installs, and whether it is
  the same `linux-image-rpi-v8` series. The M5Stack modules must be built for
  the exact running kernel.
- Whether m5stack-linux-dtoverlays builds against that kernel.
- DietPi masks `systemd-logind` and leaves out dbus by default, according to
  DietPi forum threads. The launcher's user service, lingering and PipeWire
  all depend on both.

## Phases

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
2. Add a bootstrap path for a device with no stock launcher: create
   `/usr/share/APPLaunch/{bin,share,applications}`, install the binary and
   `share/`, install the user unit, enable it. Put this in the repo as a
   script beside `scripts/device_install.sh`; do not do it by hand.
3. Mask `getty@tty1` so the console does not draw over the launcher.

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

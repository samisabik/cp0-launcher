# cp0-launcher

A personal launcher for the M5Stack CardputerZero: a terminal-styled home screen
and a small set of built-in apps sharing one theme. Forked from
[CardputerZero/launcher](https://github.com/CardputerZero/launcher) (MIT).

<img src="docs/screenshots/lockscreen.gif" alt="Lock screen" width="320">

| | |
|---|---|
| <img src="docs/screenshots/home.png" width="320"><br>Home | <img src="docs/screenshots/home-2.png" width="320"><br>Home, page 2 |
| <img src="docs/screenshots/files.png" width="320"><br>Files | <img src="docs/screenshots/files-preview.png" width="320"><br>Files: preview |
| <img src="docs/screenshots/rec.png" width="320"><br>Rec: recordings with transcripts | <img src="docs/screenshots/rec-clip.png" width="320"><br>Rec: clip |
| <img src="docs/screenshots/spectrum.png" width="320"><br>Spectrum | <img src="docs/screenshots/sdr.png" width="320"><br>SDR (waiting for a dongle) |
| <img src="docs/screenshots/audio.png" width="320"><br>Audio | <img src="docs/screenshots/settings.png" width="320"><br>Settings |
| <img src="docs/screenshots/settings-battery.png" width="320"><br>Settings: battery | <img src="docs/screenshots/settings-updates.png" width="320"><br>Settings: apt update and upgrade |

Screenshots are captured from the device framebuffer at its native 320x170.

## Install on a CardputerZero

Starting from the stock M5Stack image, on the device (CLI app or SSH):

```bash
curl -LO https://github.com/samisabik/cp0-launcher/releases/latest/download/cp0-launcher-arm64.tar.gz
tar xzf cp0-launcher-arm64.tar.gz
cd cp0-launcher && ./install.sh
```

`install.sh` replaces the launcher binary and its fonts and icons in
`/usr/share/APPLaunch`, holds the `applaunch` package and masks the stock
updaters so they do not put the stock launcher back, then restarts the launcher.

To go back to the stock launcher:

```bash
sudo cp -p /usr/share/APPLaunch/bin/M5CardputerZero-APPLaunch.stock /usr/share/APPLaunch/bin/M5CardputerZero-APPLaunch
sudo apt-mark unhold applaunch
sudo systemctl unmask applaunch-updater.service applaunch-apt-update.service applaunch-apps-updater.service
systemctl --user restart APPLaunch.service
```

## Build from source

Cross builds use a sysroot made from a device over SSH, or from a Debian trixie
arm64 container without one:

```bash
scripts/make_sysroot.sh cp0        # or: scripts/make_sysroot.sh --docker
cd projects/APPLaunch
CONFIG_DEFAULT_FILE=mac_cross_cp0_config_defaults.mk CONFIG_REPO_AUTOMATION=y \
CONFIG_TOOLCHAIN_PATH=/opt/homebrew/bin CONFIG_GCC_DUMPMACHINE=aarch64-linux-gnu \
uvx --from scons --with parse --with requests --with tqdm scons -j8
```

That is macOS with Homebrew's `aarch64-unknown-linux-gnu` toolchain; on Linux use
`linux_x86_cross_cp0_config_defaults.mk` with `gcc-aarch64-linux-gnu`.
`scripts/package_release.sh` then builds `release/cp0-launcher-arm64.tar.gz`.

Tests: `sh projects/APPLaunch/tests/run_tests.sh` and
`sh ext_components/cp0_lvgl/tests/run_tests.sh`. `AGENTS.md` is the detailed
guide to the code.

## License

MIT, see `LICENSE`. Third-party notices are in `LICENSES/` and
`docs/OPEN_SOURCE_COMPONENTS.md`.

# cp0-launcher

A personal launcher for the M5Stack CardputerZero (320x170, aarch64 Debian trixie).
Forked from [CardputerZero/launcher](https://github.com/CardputerZero/launcher)
(MIT) and slimmed down to a terminal-styled home screen with a small set of
built-in apps sharing one theme.

## Apps

Built-in pages are registered in `BUILTIN_APPS[]` in
`projects/APPLaunch/main/ui/builtin_app_registry.cpp`; each lives in
`projects/APPLaunch/main/ui/page_app/`.

- **Files**: file browser with preview, rename, copy/move, delete, and `e` to edit in micro
- **SDR**: RTL-SDR scope and waterfall with built-in WFM/NFM/AM audio
- **Spectrum**, **Rec** (recorder with whisper transcription), **Audio**
- **Settings**, **CLI**, **Python**, **SSH**, **IP Panel**

Any `.desktop` file dropped into `/usr/share/APPLaunch/applications/` also shows on the home screen.

The shared look (palette, DepartureMono font, title row, panels, footer) is in
`ext_components/cp0_lvgl/include/cp0_theme.hpp`.

## Layout

```
projects/APPLaunch/       launcher (SCons)
projects/LaunchWizard/    first-boot wizard
ext_components/cp0_lvgl/  platform layer: LVGL runner, page bases, cp0_signal_* services
scripts/                  sysroot, packaging, release helpers
SDK/                      M5Stack_Linux_Libs build system (submodule)
```

`AGENTS.md` is the detailed working guide.

## Build

Cross builds use a sysroot made from the device:

```bash
scripts/make_sysroot.sh cp0          # ssh host; writes ./sysroot
scripts/make_sysroot.sh --docker     # without a device (Debian trixie arm64 container)
```

macOS (Homebrew `aarch64-unknown-linux-gnu` toolchain):

```bash
cd projects/APPLaunch
CONFIG_DEFAULT_FILE=mac_cross_cp0_config_defaults.mk CONFIG_REPO_AUTOMATION=y \
CONFIG_TOOLCHAIN_PATH=/opt/homebrew/bin CONFIG_GCC_DUMPMACHINE=aarch64-linux-gnu \
uvx --from scons --with parse --with requests --with tqdm scons -j8
```

Linux uses `linux_x86_cross_cp0_config_defaults.mk` with `gcc-aarch64-linux-gnu`.
The output is `projects/APPLaunch/dist/M5CardputerZero-APPLaunch`.

## Test

```bash
sh projects/APPLaunch/tests/run_tests.sh
sh ext_components/cp0_lvgl/tests/run_tests.sh
```

## Deploy

```bash
aarch64-unknown-linux-gnu-strip -o /tmp/APPLaunch projects/APPLaunch/dist/M5CardputerZero-APPLaunch
scp /tmp/APPLaunch cp0:/tmp/APPLaunch
ssh cp0 'sudo install -m 755 /tmp/APPLaunch /usr/share/APPLaunch/bin/M5CardputerZero-APPLaunch && systemctl --user restart APPLaunch.service'
```

The `applaunch` package is held on the device; the launcher is updated from this repo only.

## License

MIT, see `LICENSE`. Third-party notices are in `LICENSES/` and `docs/OPEN_SOURCE_COMPONENTS.md`.

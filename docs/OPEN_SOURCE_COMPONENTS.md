# Open-source and third-party components

Inventory checked against this checkout on 2026-09-09. Build references distinguish linked dependencies from optional SDK downloads; this is not a binary SBOM. M5Stack SPDX headers and the root MIT license apply to M5Stack-authored code, without replacing upstream, font or submodule notices.

## Project code

The [root MIT license](../LICENSE) uses `2026 M5Stack Technology CO LTD`. A copy is in `ext_components/cp0_lvgl/LICENSE`.

## Embedded, fetched and linked source dependencies

Paths in the evidence column are relative to the repository root.

| Component | Build/source evidence | License and local text |
| --- | --- | --- |
| M5Stack Linux Libs SDK | `SDK`, pinned gitlink; SCons/Kconfig framework | [MIT, 2024 M5Stack](../LICENSES/M5Stack-SDK-MIT.txt) |
| LVGL 9.5 | SDK LVGL component, emulator `lib/lvgl`, APPLaunch `main/lvgl/lv_freetype*` overrides | [MIT](../LICENSES/LVGL-MIT.txt); bundled sublibraries retain their own notices |
| FreeType | linked by APPLaunch `main/lvgl/lv_freetype*` through `main/SConstruct` | FTL or GPL-2.0; [FTL text](../projects/APPLaunch/main/lvgl/freetype-LICENSE.txt) |
| tree.hh 3.20 | `ext_components/cp0_lvgl/include/tree.hh`, Kasper Peeters 2001–2024 | [GPL-3.0-only](../LICENSES/tree.hh-GPL-3.0-only.txt): header specifies version 3 without an “or later” grant |
| eventpp | cp0_lvgl and APPLaunch requirements; SDK eventpp component | [Apache-2.0](../LICENSES/eventpp-Apache-2.0.txt) |
| Departure Mono | `projects/APPLaunch/APPLaunch/share/font` | [OFL-1.1](../projects/APPLaunch/APPLaunch/share/font/DepartureMono-OFL.txt) |

LVGL embeds separately licensed codecs, renderers and fonts. Preserve its `src/libs/*/LICENSE*`, `src/stdlib/builtin/LICENSE*` and `scripts/built_in_font/font_license/` records when distributing those features. Generated font arrays retain the input font's license.

## Platform dependencies

Component SConstruct files and application SConstruct/CMake files resolve these from the host or target sysroot. Preserve version-specific notices from the packages actually shipped.

| Dependency | Use | License family |
| --- | --- | --- |
| SDL2, SDL2_image | Simulator/emulator display and input | zlib |
| GLib, GObject, GIO | Native service/event integration | LGPL-2.1-or-later |
| libdbus; BlueZ daemon | Bluetooth IPC | D-Bus AFL/GPL dual terms; BlueZ GPL/LGPL per component |
| libpulse.so.0; pipewire-pulse daemon | Speaker volume/mute through the PulseAudio-compatible server (`dlopen`, no build-time dependency) | libpulse LGPL-2.1-or-later; PipeWire MIT (Expat) with LGPL-2.1+ parts |
| NetworkManager daemon and `nmcli` | Wi-Fi, run as a separate process | GPL-2.0-or-later |
| libinput, xkbcommon, udev | Native input/device discovery | MIT for libinput/xkbcommon; udev LGPL terms |
| libcrypt, libc, libstdc++ | Account setup and runtime | Package-specific LGPL/GPL with runtime exceptions |

Packaging must preserve notices for the selected configuration, including static libraries, fonts and audio. tree.hh GPL terms must be checked against the shipped artifact; this source inventory does not resolve those release-specific questions.

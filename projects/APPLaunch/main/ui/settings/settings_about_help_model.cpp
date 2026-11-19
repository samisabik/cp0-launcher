/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "settings_about_help_model.hpp"

#include <cstddef>

namespace settings_t12b::about_help {
namespace {

std::string value_or_unknown(std::string_view value)
{
    return value.empty() ? "unknown" : std::string(value);
}


std::vector<std::string> split_lines(std::string_view text)
{
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find('\n', begin);
        if (end == std::string_view::npos) {
            lines.emplace_back(text.substr(begin));
            break;
        }
        lines.emplace_back(text.substr(begin, end - begin));
        begin = end + 1;
    }
    return lines;
}

} // namespace

Content about(std::string_view version,
              std::string_view build_date,
              std::string_view channel,
              std::string_view commit)
{
    return {
        "About",
        {
            "M5CardputerZero",
            "LVGL: 9.x",
            "Version: " + value_or_unknown(version),
            "Build: " + value_or_unknown(build_date),
            "Channel: " + value_or_unknown(channel),
            "Commit: " + value_or_unknown(commit),
        },
    };
}

Content credit()
{
    return {
        "licenses",
        split_lines(R"CP0_LICENSES(Thanks to the Kickstarter backers and the early community contributors who helped make CardputerZero possible.

This device runs the following open-source software and fonts.

- System

Debian 13 trixie / Raspberry Pi OS
https://www.debian.org
Linux kernel, GPL-2.0-only WITH Linux-syscall-note
https://github.com/raspberrypi/linux
Raspberry Pi firmware, mixed licenses
https://github.com/raspberrypi/firmware
M5Stack Linux overlays, GPL-2.0 / BSD-2-Clause
https://github.com/m5stack/m5stack-linux-dtoverlays

- Launcher

M5Stack Linux Libs, MIT
https://github.com/m5stack/M5Stack_Linux_Libs
LVGL 9.5, MIT
https://github.com/lvgl/lvgl
FreeType, FTL or GPL-2.0-only
https://gitlab.freedesktop.org/freetype/freetype
libinput, MIT
https://gitlab.freedesktop.org/libinput/libinput
libxkbcommon, MIT
https://github.com/xkbcommon/libxkbcommon
libudev, LGPL-2.1-or-later
https://github.com/systemd/systemd
D-Bus, AFL-2.1 or GPL-2.0-or-later
https://gitlab.freedesktop.org/dbus/dbus
GLib, LGPL-2.1-or-later
https://gitlab.gnome.org/GNOME/glib
miniaudio, Unlicense or MIT-0
https://github.com/mackron/miniaudio
eventpp, Apache-2.0
https://github.com/wqking/eventpp
sigslot, MIT
https://github.com/palacaze/sigslot
tree.hh, GPL-3.0-only
https://github.com/kpeeters/tree.hh

- Services and tools

NetworkManager, GPL-2.0-or-later
https://gitlab.freedesktop.org/NetworkManager/NetworkManager
BlueZ, GPL-2.0-or-later / LGPL-2.1-or-later
https://github.com/bluez/bluez
PipeWire and WirePlumber, MIT
https://gitlab.freedesktop.org/pipewire
ALSA utilities, GPL-2.0-or-later
https://github.com/alsa-project/alsa-utils
FFmpeg, LGPL-2.1-or-later / GPL-2.0-or-later
https://ffmpeg.org
whisper.cpp and ggml, MIT
https://github.com/ggml-org/whisper.cpp
Whisper tiny.en model, MIT
https://github.com/openai/whisper
Tailscale, BSD-3-Clause
https://github.com/tailscale/tailscale
rtl_433 data format, GPL-2.0-or-later
https://github.com/merbanan/rtl_433

- Icons

Pixelarticons, MIT
https://github.com/halfmage/pixelarticons
Pixel Icon Library by HackerNoon, CC BY 4.0
https://github.com/hackernoon/pixel-icon-library
Memory icons by Pictogrammers, Apache-2.0
https://github.com/Pictogrammers/Memory
1-bit Pixel Icons by Nikoichu, CC0-1.0
https://nikoichu.itch.io/pixel-icons

- Fonts

Departure Mono, SIL OFL-1.1
https://departuremono.com
DejaVu, Bitstream Vera and Arev licenses
https://github.com/dejavu-fonts/dejavu-fonts
Montserrat, SIL OFL-1.1
https://github.com/JulietaUla/Montserrat)CP0_LICENSES"),
    };
}

} // namespace settings_t12b::about_help

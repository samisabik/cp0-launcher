/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "hal_lvgl_bsp.h"

#include <cstdio>
#include <ctime>
#include <string>

// Launcher preferences stored through cp0_signal_config_api, shared by Settings,
// the lock screen and the low-battery cues.
namespace settings_prefs {

inline constexpr const char *kClock12h = "clock_12h";
inline constexpr const char *kSystemSounds = "system_sounds";

inline int get_int(const char *key, int fallback)
{
    int value = fallback;
    try {
        cp0_signal_config_api({"GetInt", key, std::to_string(fallback)}, [&value](int code, std::string data) {
            if (code != 0 || data.empty()) return;
            try {
                std::size_t used = 0;
                const int parsed = std::stoi(data, &used);
                if (used == data.size()) value = parsed;
            } catch (...) {
            }
        });
    } catch (...) {
    }
    return value;
}

// Writes and saves the value; on a failed save the previous value is restored
// so a later read cannot observe an unpersisted setting.
inline bool set_int(const char *key, int value)
{
    const int previous = get_int(key, value);
    bool written = false;
    bool saved = false;
    try {
        cp0_signal_config_api({"SetInt", key, std::to_string(value)},
                              [&written](int code, std::string) { written = code == 0; });
        if (written) cp0_signal_config_api({"Save"}, [&saved](int code, std::string) { saved = code == 0; });
        if (written && !saved) {
            cp0_signal_config_api({"SetInt", key, std::to_string(previous)}, [](int, std::string) {});
            cp0_signal_config_api({"Save"}, [](int, std::string) {});
        }
    } catch (...) {
        return false;
    }
    return written && saved;
}

inline bool clock_12h()
{
    return get_int(kClock12h, 0) != 0;
}

// "14:05" or "2:05 pm"; with seconds "14:05:09" or "2:05:09 pm".
inline std::string format_clock(const std::tm &local, bool seconds = false)
{
    char text[24];
    if (!clock_12h()) {
        if (seconds)
            std::snprintf(text, sizeof(text), "%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
        else
            std::snprintf(text, sizeof(text), "%02d:%02d", local.tm_hour, local.tm_min);
        return text;
    }
    const int hour = local.tm_hour % 12 == 0 ? 12 : local.tm_hour % 12;
    const char *suffix = local.tm_hour < 12 ? "am" : "pm";
    if (seconds)
        std::snprintf(text, sizeof(text), "%d:%02d:%02d %s", hour, local.tm_min, local.tm_sec, suffix);
    else
        std::snprintf(text, sizeof(text), "%d:%02d %s", hour, local.tm_min, suffix);
    return text;
}

inline std::string format_now(bool seconds = false)
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (!localtime_r(&now, &local)) return "--:--";
    return format_clock(local, seconds);
}

inline bool system_sounds_enabled()
{
    return get_int(kSystemSounds, 1) != 0;
}

// The one gate for launcher UI cues.
inline void play_system_sound(const char *asset)
{
    if (!asset || !asset[0] || !system_sounds_enabled()) return;
    try {
        cp0_signal_system_play(asset);
    } catch (...) {
    }
}

} // namespace settings_prefs

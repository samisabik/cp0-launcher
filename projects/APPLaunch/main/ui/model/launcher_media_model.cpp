/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "launcher_media_model.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <system_error>
#include <utility>

bool LauncherMediaControlsModel::parse_int(const std::string &text, int &value)
{
    if (text.empty()) return false;
    int parsed = 0;
    const char *begin = text.data();
    const char *end = begin + text.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    if (result.ec != std::errc() || result.ptr != end) return false;
    value = parsed;
    return true;
}

bool LauncherMediaControlsModel::parse_percent(const std::string &text, int &value)
{
    int parsed = 0;
    if (!parse_int(text, parsed) || parsed < 0 || parsed > 100) return false;
    value = parsed;
    return true;
}

int LauncherMediaControlsModel::clamp_percent(int value)
{
    return std::clamp(value, 0, 100);
}

int LauncherMediaControlsModel::volume_or(int fallback) const
{
    return volume_ >= 0 ? volume_ : clamp_percent(fallback);
}

int LauncherMediaControlsModel::brightness_or(int fallback) const
{
    return brightness_ >= 0 ? brightness_ : clamp_percent(fallback);
}

void LauncherMediaControlsModel::set_volume(int percent)
{
    volume_ = clamp_percent(percent);
}

void LauncherMediaControlsModel::set_brightness(int percent)
{
    brightness_ = clamp_percent(percent);
}

bool LauncherMediaControlsModel::toggle_mute()
{
    muted_ = !muted_;
    return muted_;
}

void LauncherMediaControlsModel::set_mute(bool muted)
{
    muted_ = muted;
}

void LauncherMediaOsdModel::show_level(std::string title, std::string icon, int percent)
{
    state_.mode = LauncherMediaOsdMode::LEVEL;
    state_.title = std::move(title);
    state_.icon = std::move(icon);
    state_.percent = LauncherMediaControlsModel::clamp_percent(percent);
    state_.visible = true;
}

void LauncherMediaOsdModel::show_mute(bool muted, std::string mute_icon, std::string volume_icon)
{
    state_.mode = LauncherMediaOsdMode::MUTE;
    state_.title = muted ? "Muted" : "Sound On";
    state_.icon = muted ? std::move(mute_icon) : std::move(volume_icon);
    state_.visible = true;
}

void LauncherMediaOsdModel::hide()
{
    state_.visible = false;
}

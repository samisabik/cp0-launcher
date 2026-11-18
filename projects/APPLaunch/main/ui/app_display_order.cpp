/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app_display_order.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace {

constexpr std::array<std::string_view, 8> kBuiltInAppOrder = {
    "settings", "cli", "python", "ssh", "ippanel", "files", "rec", "audio",
};

std::string normalized_label(std::string_view label)
{
    std::string normalized;
    normalized.reserve(label.size());
    for (const unsigned char character : label) {
        if (std::isalnum(character))
            normalized.push_back(static_cast<char>(std::tolower(character)));
    }
    return normalized;
}

// Home order for the built-in apps, grouped by type. The home grid shows six
// per page, so each row of six below is one page: system, radio, then
// hardware and network, then sound.
constexpr std::array<std::string_view, 21> kHomeOrder = {
    "settings", "files",  "cli",  "python",  "ssh",      "ippanel",
    "sdr",      "adsb",   "sat",  "sensors", "spectrum", "keys",
    "fox",      "ir",     "pins", "tailnet", "air",      "slime",
    "rec",      "audio",  "tracker",
};

} // namespace

int launcher_home_first_page_order(std::string_view label) noexcept
{
    try {
        const std::string normalized = normalized_label(label);
        for (std::size_t index = 0; index < kHomeOrder.size(); ++index)
            if (normalized == kHomeOrder[index]) return static_cast<int>(index);
    } catch (...) {
    }
    return -1;
}

int launcher_builtin_app_display_order(std::string_view label) noexcept
{
    try {
        const std::string normalized = normalized_label(label);
        for (std::size_t index = 0; index < kBuiltInAppOrder.size(); ++index)
            if (normalized == kBuiltInAppOrder[index]) return static_cast<int>(index);
    } catch (...) {
        // An allocation failure must not prevent the launcher from showing apps.
    }
    return -1;
}

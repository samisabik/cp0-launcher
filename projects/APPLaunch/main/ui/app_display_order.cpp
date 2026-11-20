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

// Home order and type of the built-in apps. The home screen lists the types
// in a rail and shows the apps of one type at a time, in this order.
struct HomeApp {
    std::string_view label;
    int category;
};
constexpr std::array<const char *, kHomeCategoryCount> kCategoryNames = {
    "system", "network", "radio", "hardware", "sound", "play", "apps",
};
constexpr std::array<HomeApp, 24> kHomeOrder = {{
    {"settings", 0}, {"files", 0},   {"cli", 0},     {"python", 0}, {"power", 0},
    {"ssh", 1},      {"ippanel", 1}, {"tailnet", 1}, {"air", 1}, {"portal", 1},
    {"sdr", 2},      {"adsb", 2},    {"sat", 2},     {"sensors", 2}, {"spectrum", 2}, {"keys", 2}, {"fox", 2},
    {"ir", 3},       {"pins", 3},
    {"rec", 4},      {"audio", 4},   {"tracker", 4}, {"pd", 4},
    {"slime", 5},
}};

} // namespace

int launcher_home_first_page_order(std::string_view label) noexcept
{
    try {
        const std::string normalized = normalized_label(label);
        for (std::size_t index = 0; index < kHomeOrder.size(); ++index)
            if (normalized == kHomeOrder[index].label) return static_cast<int>(index);
    } catch (...) {
    }
    return -1;
}

int launcher_home_category(std::string_view label) noexcept
{
    const int order = launcher_home_first_page_order(label);
    return order >= 0 ? kHomeOrder[static_cast<std::size_t>(order)].category : kHomeCategoryCount - 1;
}

const char *launcher_home_category_name(int category) noexcept
{
    return category >= 0 && category < kHomeCategoryCount ? kCategoryNames[static_cast<std::size_t>(category)] : "";
}

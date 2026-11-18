/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "../main/ui/app_display_order.hpp"

#include <array>
#include <cassert>
#include <string_view>

int main()
{
    constexpr std::array<std::string_view, 8> expected = {
        "Settings", "CLI", "Python", "SSH", "IP Panel", "Files", "Rec", "Audio",
    };

    for (std::size_t index = 0; index < expected.size(); ++index)
        assert(launcher_builtin_app_display_order(expected[index]) == static_cast<int>(index));

    // Desktop files in the existing image use this spelling for IP Panel.
    assert(launcher_builtin_app_display_order("IP_PANEL") == 4);
    assert(launcher_builtin_app_display_order("Vim") == -1);
    assert(launcher_home_first_page_order("Settings") == 0);
    assert(launcher_home_first_page_order("Files") == 1);
    assert(launcher_home_first_page_order("IP Panel") == 5);
    // Apps of one type are adjacent: system, network, radio, hardware,
    // sound, play.
    assert(launcher_home_first_page_order("Air") == 7);
    assert(launcher_home_first_page_order("Portal") == 8);
    assert(launcher_home_first_page_order("SDR") == 9);
    assert(launcher_home_first_page_order("Fox") == 15);
    assert(launcher_home_first_page_order("Pins") == 17);
    assert(launcher_home_first_page_order("Rec") == 18);
    assert(launcher_home_first_page_order("Slime") == 21);
    assert(launcher_home_first_page_order("Vim") == -1);

    assert(launcher_home_category("Settings") == 0);
    assert(launcher_home_category("IP_PANEL") == 1);
    assert(launcher_home_category("Portal") == 1);
    assert(launcher_home_category("Sensors") == 2);
    assert(launcher_home_category("IR") == 3);
    assert(launcher_home_category("Tracker") == 4);
    assert(launcher_home_category("Slime") == 5);
    // Anything else, such as a .desktop app, lands in the last type.
    assert(launcher_home_category("Vim") == kHomeCategoryCount - 1);
    assert(std::string_view(launcher_home_category_name(2)) == "radio");
    assert(std::string_view(launcher_home_category_name(kHomeCategoryCount - 1)) == "apps");
    assert(std::string_view(launcher_home_category_name(-1)).empty());
    assert(std::string_view(launcher_home_category_name(kHomeCategoryCount)).empty());

    return 0;
}

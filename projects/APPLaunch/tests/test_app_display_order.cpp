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
    // Each page of six is one type: radio starts the second page, hardware
    // and network the third, sound the fourth.
    assert(launcher_home_first_page_order("SDR") == 6);
    assert(launcher_home_first_page_order("Keys") == 11);
    assert(launcher_home_first_page_order("Fox") == 12);
    assert(launcher_home_first_page_order("Rec") == 18);
    assert(launcher_home_first_page_order("Tracker") == 20);
    assert(launcher_home_first_page_order("Vim") == -1);

    return 0;
}

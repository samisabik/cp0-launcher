/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "../main/ui/app_display_order.hpp"

#include <cassert>
#include <string_view>

int main()
{
    assert(launcher_home_first_page_order("Settings") == 0);
    assert(launcher_home_first_page_order("Files") == 1);
    assert(launcher_home_first_page_order("Power") == 4);
    assert(launcher_home_first_page_order("IP Panel") == 6);
    // Apps of one type are adjacent: system, network, radio, hardware,
    // sound, play.
    assert(launcher_home_first_page_order("Air") == 8);
    assert(launcher_home_first_page_order("Portal") == 9);
    assert(launcher_home_first_page_order("SDR") == 10);
    assert(launcher_home_first_page_order("Fox") == 16);
    assert(launcher_home_first_page_order("Pins") == 18);
    assert(launcher_home_first_page_order("Rec") == 19);
    assert(launcher_home_first_page_order("Slime") == 22);
    assert(launcher_home_first_page_order("Vim") == -1);

    assert(launcher_home_category("Settings") == 0);
    assert(launcher_home_category("Power") == 0);
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

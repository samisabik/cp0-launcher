/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "../main/ui/settings/settings_about_help_model.hpp"
#include "../main/ui/settings/settings_boot_action_policy.hpp"
#include "../main/ui/settings/settings_launcher_model.hpp"

#include <cassert>
#include <string>

int main()
{
    using settings_t12b::boot_actions::Action;
    using settings_t12b::boot_actions::Operation;
    assert(settings_t12b::boot_actions::presentation(Action::Reboot).confirmation_title == "reboot?");
    assert(settings_t12b::boot_actions::presentation(Action::Shutdown).confirmation_title == "power off?");
    assert(std::string(settings_t12b::boot_actions::process_command(Operation::Reboot)) == "Reboot");

    const auto about = settings_t12b::about_help::about("1.2.3", "2026-08-24", "stable", "abc123");
    const auto credit = settings_t12b::about_help::credit();
    assert(!about.title.empty() && about.lines.size() >= 4);
    assert(credit.title == "licenses");
    assert(credit.lines.size() > 60);
    assert(credit.lines.front().find("Kickstarter backers") != std::string::npos);
    assert(credit.lines.back() == "https://github.com/JulietaUla/Montserrat");
}

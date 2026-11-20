/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "../main/ui/settings/settings_about_help_model.hpp"
#include "../main/ui/settings/settings_boot_action_policy.hpp"

#include <cassert>
#include <string>

int main()
{
    using settings_t12b::boot_actions::Action;
    using settings_t12b::boot_actions::Operation;
    assert(settings_t12b::boot_actions::presentation(Action::Reboot).label == "reboot");
    assert(settings_t12b::boot_actions::presentation(Action::Shutdown).label == "shutdown");
    assert(std::string(settings_t12b::boot_actions::process_command(Operation::Reboot)) == "Reboot");

    const auto credit = settings_t12b::about_help::credit();
    assert(credit.title == "licenses");
    assert(credit.lines.size() > 60);
    assert(credit.lines.front().find("Kickstarter backers") != std::string::npos);
    assert(credit.lines.back() == "https://github.com/JulietaUla/Montserrat");
}

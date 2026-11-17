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

namespace {

struct Descriptor
{
    const char *label;
    const char *config_key;
    bool configurable;
    bool always_on;
    LauncherAppOrigin origin;
};

} // namespace

int main()
{
    const Descriptor descriptors[] = {
        {"Hidden", "hidden", false, true, LauncherAppOrigin::Builtin},
        {"Spectrum", "spectrum", true, false, LauncherAppOrigin::Builtin},
        {"Python", "app_Python", true, false, LauncherAppOrigin::Builtin},
        {"SDR", "app_SDR", true, false, LauncherAppOrigin::Builtin},
        {"Settings", "app_Setting", true, false, LauncherAppOrigin::Builtin},
        {"CLI", "app_CLI", true, false, LauncherAppOrigin::Builtin},
        {"Downloaded", "app_desktop_downloaded", true, false, LauncherAppOrigin::External},
        {nullptr, "missing-label", true, false, LauncherAppOrigin::Builtin},
        {"Missing key", nullptr, true, false, LauncherAppOrigin::Builtin},
    };
    const auto entries = settings_t12b::launcher::configurable_entries(
        descriptors,
        9,
        [](const Descriptor &descriptor) { return descriptor.always_on; });
    assert(entries.size() == 3);
    assert(entries[0].label == "Spectrum");
    assert(entries[0].config_key == "spectrum");
    assert(!entries[0].enabled);
    assert(entries[1].label == "Python" && entries[1].config_key == "app_Python");
    assert(entries[2].label == "SDR" && entries[2].config_key == "app_SDR");
    using settings_t12b::boot_actions::Action;
    using settings_t12b::boot_actions::Operation;
    assert(settings_t12b::boot_actions::presentation(Action::Reboot).confirmation_title == "reboot?");
    assert(settings_t12b::boot_actions::presentation(Action::Shutdown).confirmation_title == "power off?");
    const std::vector<Operation> factory_plan = {
        Operation::RemoveLauncherSettings,
        Operation::Reboot,
    };
    const std::vector<Operation> oobe_plan = {
        Operation::TouchOobeMarker,
        Operation::Reboot,
    };
    assert(settings_t12b::boot_actions::operation_plan(Action::FactoryReset) == factory_plan);
    assert(settings_t12b::boot_actions::operation_plan(Action::RearmOobe) == oobe_plan);
    assert(settings_t12b::boot_actions::may_continue(Operation::RemoveLauncherSettings, true));
    assert(!settings_t12b::boot_actions::may_continue(Operation::RemoveLauncherSettings, false));
    assert(!settings_t12b::boot_actions::may_continue(Operation::Reboot, true));
    assert(std::string(settings_t12b::boot_actions::process_command(Operation::Reboot)) == "Reboot");
    assert(std::string(settings_t12b::boot_actions::filesystem_alias(Operation::TouchOobeMarker)) ==
           "oobe_marker");

    const auto about = settings_t12b::about_help::about("1.2.3", "2026-08-24", "stable", "abc123");
    const auto credit = settings_t12b::about_help::credit();
    assert(!about.title.empty() && about.lines.size() >= 4);
    assert(credit.title == "licenses");
    assert(credit.lines.size() > 60);
    assert(credit.lines.front().find("Kickstarter backers") != std::string::npos);
    assert(credit.lines.back() == "https://github.com/JulietaUla/Montserrat");
}

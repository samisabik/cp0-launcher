/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "settings_boot_action_policy.hpp"

namespace settings_t12b::boot_actions {
namespace {

constexpr Presentation kReboot{"reboot"};
constexpr Presentation kShutdown{"shutdown"};

} // namespace

const Presentation &presentation(Action action)
{
    switch (action) {
    case Action::Reboot:
        return kReboot;
    case Action::Shutdown:
        return kShutdown;
    }
    return kReboot;
}

const char *process_command(Operation operation) noexcept
{
    switch (operation) {
    case Operation::Reboot:
        return "Reboot";
    case Operation::Shutdown:
        return "Shutdown";
    }
    return nullptr;
}

} // namespace settings_t12b::boot_actions

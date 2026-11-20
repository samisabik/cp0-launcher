/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string_view>
#include <vector>

namespace settings_t12b::boot_actions {

enum class Action {
    Reboot,
    Shutdown,
};

struct Presentation
{
    std::string_view label;
};

const Presentation &presentation(Action action);
const char *process_command(Action action) noexcept;

} // namespace settings_t12b::boot_actions

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "../app_registry.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace settings_t12b::launcher {

inline bool is_protected_launcher_app(std::string_view config_key)
{
    return config_key == "app_Setting" ||
           config_key == "app_CLI";
}

inline bool is_launcher_settings_origin(LauncherAppOrigin origin)
{
    return origin == LauncherAppOrigin::Builtin;
}

} // namespace settings_t12b::launcher

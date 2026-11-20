/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "settings_boot_action_policy.hpp"
#include "settings_tree_types.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app_registry.h"

namespace settings_t12b {

bool populate_launcher_children(Tree &tree, const NodeIter &parent);

SettingApiCallBackFunc make_launcher_toggle_api(const AppDescriptor &descriptor);

} // namespace settings_t12b

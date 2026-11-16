/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "settings_boot_action_policy.hpp"

// Covers the display with a "shutting down" / "rebooting" notice, then asks the
// system to power off or reboot.  A refused request replaces the notice with the
// reason and hands the screen back after a few seconds or on any key.
void settings_power_request(settings_t12b::boot_actions::Action action);

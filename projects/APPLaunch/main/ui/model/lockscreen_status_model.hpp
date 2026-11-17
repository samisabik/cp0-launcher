/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>

// Text for the lock screen status card, kept free of LVGL and hardware access.
namespace lockscreen_status {

// "90%  4h 10m left" while discharging, "90%  charging" while charging,
// "90%" when the current is too small to estimate. Negative avg_current_ma
// means discharge, as the BQ27220 reports it.
std::string battery_line(int soc, int remain_mah, int avg_current_ma);

// Time since boot: "12m", "1h 40m", "3d 4h".
std::string uptime_line(long seconds);

} // namespace lockscreen_status

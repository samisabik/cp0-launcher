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

// "3 clips", "3 clips, 1 to transcribe", or "none".
std::string recordings_line(int total, int untranscribed);

} // namespace lockscreen_status

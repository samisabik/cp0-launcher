/*
 * SPDX-License-Identifier: MIT
 */

#include "lockscreen_status_model.hpp"

#include <algorithm>
#include <cstdio>

namespace lockscreen_status {

std::string battery_line(int soc, int remain_mah, int avg_current_ma)
{
    char text[48];
    const int percent = std::clamp(soc, 0, 100);
    // Below 20 mA the estimate swings wildly; the device idles well above that.
    if (avg_current_ma <= -20 && remain_mah > 0) {
        const long minutes = static_cast<long>(remain_mah) * 60 / -avg_current_ma;
        if (minutes >= 60)
            std::snprintf(text, sizeof(text), "%d%%  %ldh %02ldm left", percent, minutes / 60, minutes % 60);
        else
            std::snprintf(text, sizeof(text), "%d%%  %ldm left", percent, minutes);
    } else if (avg_current_ma >= 20) {
        std::snprintf(text, sizeof(text), "%d%%  charging", percent);
    } else {
        std::snprintf(text, sizeof(text), "%d%%", percent);
    }
    return text;
}

std::string uptime_line(long seconds)
{
    if (seconds < 0) seconds = 0;
    const long minutes = seconds / 60;
    const long hours = minutes / 60;
    const long days = hours / 24;
    if (days > 0) return std::to_string(days) + "d " + std::to_string(hours % 24) + "h";
    if (hours > 0) return std::to_string(hours) + "h " + std::to_string(minutes % 60) + "m";
    return std::to_string(minutes) + "m";
}

} // namespace lockscreen_status

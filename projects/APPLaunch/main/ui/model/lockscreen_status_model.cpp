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

std::string recordings_line(int total, int untranscribed)
{
    if (total <= 0) return "none";
    std::string text = std::to_string(total) + (total == 1 ? " clip" : " clips");
    if (untranscribed > 0) text += ", " + std::to_string(untranscribed) + " to transcribe";
    return text;
}

} // namespace lockscreen_status

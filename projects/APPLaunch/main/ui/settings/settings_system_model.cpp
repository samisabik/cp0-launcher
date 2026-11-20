/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "settings_system_model.hpp"

#include <algorithm>
#include <charconv>

namespace settings_system {

UpdateStatusInfo parse_update_status(const std::string &state)
{
    UpdateStatusInfo result;
    const std::size_t separator = state.find(':');
    result.stage = state.substr(0, separator);
    result.terminal = result.stage == "succeeded" || result.stage == "failed" || result.stage == "cancelled";
    if (result.terminal || separator == std::string::npos) return result;

    int percent = 0;
    const char *begin = state.data() + separator + 1;
    const char *end = state.data() + state.size();
    const auto parsed = std::from_chars(begin, end, percent);
    if (parsed.ec == std::errc{} && parsed.ptr == end) result.progress = std::clamp(percent, 0, 100);
    return result;
}

} // namespace settings_system

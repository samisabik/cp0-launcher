/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "cp0_lvgl_app.h"

#include <functional>
#include <list>
#include <string>

namespace cp0::battery {

struct ApiReply {
    int code = -1;
    std::string data;
};

using ReadOperation = std::function<cp0_battery_info_t()>;

ApiReply dispatch_api_request(const std::list<std::string> &arguments,
                              const ReadOperation &read);

} // namespace cp0::battery

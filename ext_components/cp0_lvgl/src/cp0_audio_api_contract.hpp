/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <list>
#include <functional>
#include <string>

namespace cp0::audio {

enum class ApiCommand {
    VolumeRead,
    VolumeWrite,
    DefaultSink,
    MuteRead,
    MuteToggle,
};

struct ApiRequest
{
    ApiCommand command = ApiCommand::VolumeRead;
    int value = 0;
};

bool parse_api_request(const std::list<std::string> &args, ApiRequest &request);
const char *invalid_api_request_message();
void invoke_callback(const std::function<void(int, std::string)> &callback,
                     int code, const std::string &data) noexcept;

} // namespace cp0::audio

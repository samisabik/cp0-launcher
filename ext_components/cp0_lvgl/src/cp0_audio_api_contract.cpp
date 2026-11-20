/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "cp0_audio_api_contract.hpp"

#include <charconv>
#include <string_view>

namespace cp0::audio {
namespace {

bool parse_integer(std::string_view text, int minimum, int maximum, int &value)
{
    if (text.empty())
        return false;
    int parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        parsed < minimum || parsed > maximum)
        return false;
    value = parsed;
    return true;
}

bool is_no_argument_command(const std::string &command, ApiCommand &parsed)
{
    if (command == "VolumeRead") parsed = ApiCommand::VolumeRead;
    else if (command == "DefaultSink") parsed = ApiCommand::DefaultSink;
    else if (command == "MuteRead") parsed = ApiCommand::MuteRead;
    else if (command == "MuteToggle") parsed = ApiCommand::MuteToggle;
    else return false;
    return true;
}

} // namespace

bool parse_api_request(const std::list<std::string> &args, ApiRequest &request)
{
    request = {};
    if (args.empty())
        return false;
    auto argument = args.begin();
    const std::string &command = *argument++;

    if (is_no_argument_command(command, request.command))
        return args.size() == 1;
    if (command == "VolumeWrite") {
        request.command = ApiCommand::VolumeWrite;
        return args.size() == 2 && parse_integer(*argument, 0, 100, request.value);
    }
    return false;
}

const char *invalid_api_request_message()
{
    return "invalid audio api request\n";
}

void invoke_callback(const std::function<void(int, std::string)> &callback,
                     int code, const std::string &data) noexcept
{
    if (!callback) return;
    try {
        callback(code, data);
    } catch (...) {
    }
}

} // namespace cp0::audio

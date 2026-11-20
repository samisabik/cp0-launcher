/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "cp0_audio_api_contract.hpp"

#include <cassert>
#include <stdexcept>
#include <string>

namespace {

using cp0::audio::ApiCommand;
using cp0::audio::ApiRequest;

void test_no_argument_commands()
{
    ApiRequest request;
    assert(!cp0::audio::parse_api_request({}, request));
    assert(!cp0::audio::parse_api_request({"Unknown"}, request));
    assert(cp0::audio::parse_api_request({"VolumeRead"}, request));
    assert(request.command == ApiCommand::VolumeRead);
    assert(!cp0::audio::parse_api_request({"VolumeRead", "junk"}, request));
    assert(cp0::audio::parse_api_request({"MuteRead"}, request));
    assert(request.command == ApiCommand::MuteRead);
    assert(cp0::audio::parse_api_request({"MuteToggle"}, request));
    assert(request.command == ApiCommand::MuteToggle);
    assert(cp0::audio::parse_api_request({"DefaultSink"}, request));
    assert(request.command == ApiCommand::DefaultSink);
    assert(!cp0::audio::parse_api_request({"DefaultSink", "extra"}, request));
}

void test_numeric_commands()
{
    ApiRequest request;
    assert(cp0::audio::parse_api_request({"VolumeWrite", "0"}, request));
    assert(request.command == ApiCommand::VolumeWrite && request.value == 0);
    assert(cp0::audio::parse_api_request({"VolumeWrite", "100"}, request));
    assert(request.value == 100);
    assert(!cp0::audio::parse_api_request({"VolumeWrite"}, request));
    assert(!cp0::audio::parse_api_request({"VolumeWrite", ""}, request));
    assert(!cp0::audio::parse_api_request({"VolumeWrite", "12junk"}, request));
    assert(!cp0::audio::parse_api_request({"VolumeWrite", "-1"}, request));
    assert(!cp0::audio::parse_api_request({"VolumeWrite", "101"}, request));
    assert(!cp0::audio::parse_api_request({"VolumeWrite", "99999999999999999999"}, request));
    assert(!cp0::audio::parse_api_request({"VolumeWrite", "50", "junk"}, request));
    assert(std::string(cp0::audio::invalid_api_request_message()) ==
        "invalid audio api request\n");
}

void test_callback_exception_boundary()
{
    int calls = 0;
    cp0::audio::invoke_callback(
        [&](int code, std::string data) {
            ++calls;
            assert(code == 7 && data == "result");
            throw std::runtime_error("callback");
        },
        7, "result");
    assert(calls == 1);
    cp0::audio::invoke_callback({}, 0, "ignored");
}

} // namespace

int main()
{
    test_no_argument_commands();
    test_numeric_commands();
    test_callback_exception_boundary();
}

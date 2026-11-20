/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "cp0_battery_api_contract.hpp"
#include "cp0_callback_contract.hpp"

#include <cassert>
#include <stdexcept>

int main()
{
    using cp0::battery::dispatch_api_request;

    int read_calls = 0;
    auto reader = [&] {
        ++read_calls;
        return cp0_battery_info_t{4100, -125, 273, 81, 2430, 3000, 4, -120, 1};
    };

    auto read_reply = dispatch_api_request({"Read"}, reader);
    assert(read_reply.code == 0);
    assert(read_reply.data == "4100,-125,273,81,2430,3000,4,-120,1");
    assert(read_calls == 1);

    assert(dispatch_api_request({}, reader).code == -1);
    assert(dispatch_api_request({"Unknown"}, reader).code == -1);
    assert(dispatch_api_request({"Read", "extra"}, reader).code == -1);
    assert(read_calls == 1);

    auto invalid_read = dispatch_api_request({"Read"}, [] { return cp0_battery_info_t{}; });
    assert(invalid_read.code == -1 && invalid_read.data == "0,0,0,0,0,0,0,0,0");
    assert(dispatch_api_request({"Read"}, {}).code == -1);

    auto throwing_read = dispatch_api_request(
        {"Read"}, []() -> cp0_battery_info_t { throw std::runtime_error("read"); });
    assert(throwing_read.code == -1 && throwing_read.data == "battery backend failure");

    int callback_calls = 0;
    cp0::callback::invoke([&](int code, std::string data) {
        ++callback_calls;
        assert(code == -1 && data == "battery backend failure");
        throw std::runtime_error("callback");
    }, throwing_read.code, throwing_read.data);
    assert(callback_calls == 1);
    return 0;
}

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "cp0_battery_api_contract.hpp"

#include "cp0_battery_codec.hpp"

namespace cp0::battery {

ApiReply dispatch_api_request(const std::list<std::string> &arguments,
                              const ReadOperation &read)
{
    try {
        if (arguments.empty() || arguments.front() != "Read")
            return {-1, "unknown bq27220 api command"};
        if (arguments.size() != 1) return {-1, "invalid bq27220 Read arguments"};
        if (!read) return {-1, "battery reader unavailable"};
        const cp0_battery_info_t info = read();
        return {info.valid == 1 ? 0 : -1, encode_info(info)};
    } catch (...) {
        return {-1, "battery backend failure"};
    }
}

} // namespace cp0::battery

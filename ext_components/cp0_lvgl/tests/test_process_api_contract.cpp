/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "cp0_process_api_contract.hpp"

#include <cassert>
#include <functional>
#include <cerrno>
#include <new>
#include <list>
#include <string>

struct ProcessOperation {
    int operator()() const { return 0; }
};

int main()
{
    static_assert(noexcept(cp0_process_api_contract::invoke_c_api(ProcessOperation{})));
    assert(cp0_process_api_contract::invoke_c_api([] { return 7; }) == 7);
    assert(cp0_process_api_contract::invoke_c_api(
        []() -> int { throw std::bad_alloc(); }) == -ENOMEM);
    assert(cp0_process_api_contract::invoke_c_api(
        []() -> int { throw 1; }) == -EIO);
    cp0_process_api_contract::invoke_c_api_void([] { throw 1; });
    using namespace cp0_process_api_contract;
    const std::list<std::string> request = {"Spawn", "42", "1000"};
    assert(has_exact_arguments(request, 3));
    assert(has_at_least_arguments(request, 2));
    assert(argument_at(request, 1) && *argument_at(request, 1) == "42");
    assert(argument_at(request, 3) == nullptr);
    assert(arguments_from(request, 1) == (std::vector<std::string>{"42", "1000"}));

    bool flag = false;
    assert(parse_bool("1", flag) && flag);
    assert(parse_bool("0", flag) && !flag);
    assert(!parse_bool("2", flag));
    assert(!parse_bool("true", flag));

    int value = 0;
    assert(parse_pid("1", value) && value == 1);
    assert(!parse_pid("0", value));
    assert(!parse_pid("12junk", value));
    assert(!parse_pid(" 12", value));

    bool callback_called = false;
    invoke_callback_safely(
        std::function<void(int, std::string)>([&](int code, std::string data) {
            callback_called = code == 7 && data == "ok";
            throw 1;
        }),
        7, "ok");
    assert(callback_called);
    return 0;
}

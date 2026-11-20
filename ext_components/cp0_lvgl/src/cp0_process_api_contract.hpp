/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <charconv>
#include <cerrno>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <new>
#include <list>
#include <string>
#include <string_view>
#include <vector>

namespace cp0_process_api_contract {

template <typename Operation>
inline int invoke_c_api(Operation &&operation) noexcept
{
    try {
        return std::forward<Operation>(operation)();
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    } catch (...) {
        return -EIO;
    }
}

template <typename Operation>
inline void invoke_c_api_void(Operation &&operation) noexcept
{
    try {
        std::forward<Operation>(operation)();
    } catch (...) {
    }
}

inline bool has_exact_arguments(const std::list<std::string> &args, std::size_t count)
{
    return args.size() == count;
}

inline bool has_at_least_arguments(const std::list<std::string> &args, std::size_t count)
{
    return args.size() >= count;
}

inline const std::string *argument_at(const std::list<std::string> &args, std::size_t index)
{
    auto iterator = args.begin();
    while (index > 0 && iterator != args.end()) {
        --index;
        ++iterator;
    }
    return iterator == args.end() ? nullptr : &*iterator;
}

inline std::vector<std::string> arguments_from(const std::list<std::string> &args,
                                               std::size_t index)
{
    std::vector<std::string> result;
    auto iterator = args.begin();
    while (index > 0 && iterator != args.end()) {
        --index;
        ++iterator;
    }
    result.reserve(static_cast<std::size_t>(std::distance(iterator, args.end())));
    for (; iterator != args.end(); ++iterator) result.push_back(*iterator);
    return result;
}

template <typename Integer>
inline bool parse_integer(std::string_view text, Integer minimum, Integer maximum, Integer &value)
{
    if (text.empty()) return false;
    Integer parsed = 0;
    const char *begin = text.data();
    const char *end = begin + text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed < minimum || parsed > maximum)
        return false;
    value = parsed;
    return true;
}

inline bool parse_bool(std::string_view text, bool &value)
{
    int parsed = 0;
    if (!parse_integer(text, 0, 1, parsed)) return false;
    value = parsed != 0;
    return true;
}

inline bool parse_pid(std::string_view text, int &value)
{
    return parse_integer(text, 1, std::numeric_limits<int>::max(), value);
}

inline void invoke_callback_safely(
    const std::function<void(int, std::string)> &callback,
    int code,
    const std::string &data) noexcept
{
    if (!callback) return;
    try {
        callback(code, data);
    } catch (...) {
    }
}

} // namespace cp0_process_api_contract

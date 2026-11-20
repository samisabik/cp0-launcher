/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "cp0_async_testable_utils.hpp"

#include <cassert>
#include <string>

int main()
{
    using namespace cp0_testable;

    static_assert(is_leap_year(2000));
    static_assert(!is_leap_year(1900));
    static_assert(is_leap_year(2024));
    static_assert(!is_leap_year(2023));
    static_assert(days_in_month(2024, 2) == 29);
    static_assert(days_in_month(2023, 2) == 28);
    static_assert(days_in_month(2026, 4) == 30);
    static_assert(days_in_month(2026, 1) == 31);
    static_assert(days_in_month(2026, 0) == 0);
    static_assert(days_in_month(2026, 13) == 0);

    std::string tail;
    append_tail(tail, "abc", 3, 5);
    assert(tail == "abc");
    append_tail(tail, "def", 3, 5);
    assert(tail == "bcdef");
    append_tail(tail, "0123456789", 10, 5);
    assert(tail == "56789");
    append_tail(tail, "ignored", 7, 0);
    assert(tail.empty());

    assert(pending_output_fits(0, 256, 256));
    assert(pending_output_fits(255, 1, 256));
    assert(!pending_output_fits(255, 2, 256));
    assert(!pending_output_fits(257, 0, 256));
    static_assert(prefer_io_error(-5, 0) == -5);
    static_assert(prefer_io_error(0, 7) == 7);

    constexpr std::size_t event_limit = 64;
    constexpr std::size_t byte_limit = 64 * 1024;
    static_assert(callback_event_fits_tick(0, 0, byte_limit + 1,
                                           event_limit, byte_limit));
    static_assert(callback_event_fits_tick(63, byte_limit - 1, 1,
                                           event_limit, byte_limit));
    static_assert(!callback_event_fits_tick(63, byte_limit, 1,
                                            event_limit, byte_limit));
    static_assert(callback_event_fits_tick(63, byte_limit, 0,
                                           event_limit, byte_limit));
    static_assert(!callback_event_fits_tick(64, 0, 0,
                                            event_limit, byte_limit));
}

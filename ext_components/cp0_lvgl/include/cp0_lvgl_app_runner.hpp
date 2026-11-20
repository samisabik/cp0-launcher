/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "ui_app_page.hpp"

#include <functional>
#include <memory>
#include <utility>

struct Cp0LvglRunOptions
{
    std::function<void()> after_resource_init;
    std::function<bool()> setup;
    std::function<bool()> should_quit;
    std::function<void()> teardown;
};

int cp0_lvgl_run(Cp0LvglRunOptions options = {});

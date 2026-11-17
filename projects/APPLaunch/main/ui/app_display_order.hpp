/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string_view>

// Returns the zero-based position in the product's built-in app order, or -1
// when the label belongs to an installable application.
int launcher_builtin_app_display_order(std::string_view label) noexcept;
// Position on the first home page, or -1 when the app is not pinned there.
int launcher_home_first_page_order(std::string_view label) noexcept;

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
// Position of a built-in app in the home order, which groups apps by type,
// or -1 when the label is not one of them.
int launcher_home_first_page_order(std::string_view label) noexcept;

// App types shown in the home rail. The last one, "apps", holds everything
// that is not a built-in app with a type of its own.
inline constexpr int kHomeCategoryCount = 7;
// 0 .. kHomeCategoryCount - 1
int launcher_home_category(std::string_view label) noexcept;
// Lowercase rail label; empty for an index out of range.
const char *launcher_home_category_name(int category) noexcept;

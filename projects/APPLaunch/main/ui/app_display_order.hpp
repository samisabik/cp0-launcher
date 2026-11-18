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

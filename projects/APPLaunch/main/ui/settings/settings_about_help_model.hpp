/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace settings_t12b::about_help {

struct Content
{
    std::string title;
    std::vector<std::string> lines;
};

Content credit();

} // namespace settings_t12b::about_help

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "cp0_lvgl_app.h"

#include <cstddef>
#include <string>

namespace cp0_posix_filesystem {

int encode_directory(const char *path, std::string &output);
int read_file_limited(const std::string &path, size_t max_bytes, std::string &output);

} // namespace cp0_posix_filesystem

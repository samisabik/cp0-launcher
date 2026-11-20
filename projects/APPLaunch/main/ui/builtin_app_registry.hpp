/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <list>
#include <string>

#include "app_registry.h"

struct app;

void launcher_append_enabled_builtin_apps(std::list<app> &apps);
void launcher_sort_app_display_order(std::list<app> &apps);
bool launcher_builtin_app_owns_exec(const std::string &exec);

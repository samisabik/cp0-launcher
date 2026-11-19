/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "cp0_lvgl_log.h"

#include <cstdarg>
#include <cstdio>

void cp0_log(const char *topic, const char *message)
{
    std::fprintf(stderr, "[%s] %s\n", topic && topic[0] ? topic : "log", message ? message : "");
}

void cp0_logf(const char *topic, const char *fmt, ...)
{
    if (!fmt) return;
    char buf[1024] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    cp0_log(topic, buf);
}

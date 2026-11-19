/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

// Writes "[topic] message" to stderr, which the service manager journals.
void cp0_log(const char *topic, const char *message);
void cp0_logf(const char *topic, const char *fmt, ...);

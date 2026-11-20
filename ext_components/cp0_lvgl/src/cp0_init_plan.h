/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cp0_init_step {
    CP0_INIT_STEP_ENV,
    CP0_INIT_STEP_EVENT,
    CP0_INIT_STEP_FILESYSTEM,
    CP0_INIT_STEP_CONFIG,
    CP0_INIT_STEP_PTY,
    CP0_INIT_STEP_DISPLAY,
    CP0_INIT_STEP_INPUT,
    CP0_INIT_STEP_AUDIO,
    CP0_INIT_STEP_PROCESS,
    CP0_INIT_STEP_SUDO,
    CP0_INIT_STEP_OSINFO,
    CP0_INIT_STEP_SCREENSHOT,
    CP0_INIT_STEP_WIFI,
    CP0_INIT_STEP_BLUETOOTH,
    CP0_INIT_STEP_SETTINGS,
    CP0_INIT_STEP_BQ27220,
    CP0_INIT_STEP_SAVED_SETTINGS,
    CP0_INIT_STEP_BATTERY,
    CP0_INIT_STEP_COUNT,
} cp0_init_step_t;

#ifdef __cplusplus
}
#endif

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CP0_C_EVENT_BATTERY = 0,
    CP0_C_EVENT_END,
} CP0_C_EVENT_t;

extern uint32_t lv_c_event[(2*CP0_C_EVENT_END)];

void cp0_lvgl_init(void);
#ifdef __cplusplus
}
#include "eventpp/callbacklist.h"
#include <list>
#include <string>
#include <functional>

#define def_hal_fun(arg, name) extern eventpp::CallbackList<arg> name;
#include "signal_register_plan.h"
#undef def_hal_fun

std::string cp0_file_path(std::string file);
#endif

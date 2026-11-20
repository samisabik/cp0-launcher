/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "hal_lvgl_bsp.h"

#include "lvgl/lvgl.h"

#include "keyboard_input.h"
#include "cp0_lvgl_app.h"
#include "cp0_font_service.hpp"

#ifdef __cplusplus
#include <memory>
#include <string>
#include <unordered_map>

namespace launcher_ui {

namespace events {
inline const struct key_item *keyboard_item(lv_event_t *event)
{
    return static_cast<const struct key_item *>(lv_event_get_param(event));
}

inline uint32_t keyboard_key(lv_event_t *event)
{
    const struct key_item *item = keyboard_item(event);
    return item ? item->key_code : 0;
}

inline int keyboard_state(lv_event_t *event)
{
    const struct key_item *item = keyboard_item(event);
    return item ? item->key_state : 0;
}

inline bool is_key_pressed(lv_event_t *event)
{
    return lv_event_get_code(event) == static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD) &&
           keyboard_state(event) > 0;
}

inline bool is_key_released(lv_event_t *event)
{
    return lv_event_get_code(event) == static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD) &&
           keyboard_state(event) == 0;
}
} // namespace events

void init();
void deinit();
}

inline Cp0FontService &launcher_fonts() { return cp0_fonts(); }
#ifndef APP_PAGE_IMPLEMENTATION_UNIT
#include "launch.h"
#include "ui_launch_page.h"
#include "launcher_ui_runtime.h"
#endif
#endif

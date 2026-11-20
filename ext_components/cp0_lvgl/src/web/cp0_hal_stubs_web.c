/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "keyboard_input.h"

static cp0_keyboard_key_handler_t global_key_handler;
static int lvgl_keypad_intercept;

void cp0_keyboard_set_global_key_handler(cp0_keyboard_key_handler_t handler)
{
    global_key_handler = handler;
}

void cp0_keyboard_set_lvgl_keypad_intercept(int intercept)
{ lvgl_keypad_intercept = intercept != 0; }

int cp0_keyboard_get_lvgl_keypad_intercept(void)
{ return lvgl_keypad_intercept; }

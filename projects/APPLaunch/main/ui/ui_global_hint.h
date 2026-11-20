/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

struct key_item;

// Global launcher shortcut dispatcher for the launcher and sub-app pages.
// Key classification lives in GlobalHintPolicy; this module executes the
// resulting media, screenshot, and transient hint actions.
//
// Registered with cp0_keyboard_set_global_key_handler, so on_key runs after
// LV_EVENT_KEYBOARD has been sent to the active screen. It only reads elm.
namespace ui_global_hint {
void on_key(const struct key_item *elm) noexcept;
void shutdown();
}

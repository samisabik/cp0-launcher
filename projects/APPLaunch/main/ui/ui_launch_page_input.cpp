/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "ui_launch_page.h"

#include "input_keys.h"
#include "keyboard_input.h"
#include "ui_screensaver.h"

namespace {

uint32_t fzxc_to_arrow(uint32_t key)
{
    switch (key) {
    case KEY_F:
        return KEY_UP;
    case KEY_X:
        return KEY_DOWN;
    case KEY_Z:
        return KEY_LEFT;
    case KEY_C:
        return KEY_RIGHT;
    default:
        return key;
    }
}

} // namespace

void UILaunchPage::handle_home_key(lv_event_t *event)
{
    if (!event) return;
    const auto *elm = static_cast<const struct key_item *>(lv_event_get_param(event));
    if (!elm) return;

    // Text editors own printable keys, including F/X/Z/C.
    if (elm->input_context == KBD_INPUT_CONTEXT_TEXT ||
        cp0_keyboard_get_input_context() == KBD_INPUT_CONTEXT_TEXT)
        return;

    const uint32_t code = elm->semantic_key ? elm->semantic_key : fzxc_to_arrow(elm->key_code);

    if (elm->key_state == KBD_KEY_PRESSED || elm->key_state == KBD_KEY_REPEATED) {
        switch (code) {
        case KEY_LEFT:
            move_selection(-1, 0);
            break;
        case KEY_RIGHT:
            move_selection(1, 0);
            break;
        case KEY_UP:
            move_selection(0, -1);
            break;
        case KEY_DOWN:
            move_selection(0, 1);
            break;
        case KEY_TAB:
            if (elm->key_state == KBD_KEY_PRESSED) ui_screensaver_lock_now();
            break;
        default:
            break;
        }
    } else if (elm->key_state == KBD_KEY_RELEASED && code == KEY_ENTER) {
        activate();
    }
}

void UILaunchPage::on_home_key(lv_event_t *event) noexcept
{
    try {
        if (auto *self = static_cast<UILaunchPage *>(lv_event_get_user_data(event)))
            self->handle_home_key(event);
    } catch (...) {
    }
}

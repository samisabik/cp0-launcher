/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "python_terminal_help.hpp"

#include "cp0_font_service.hpp"

lv_obj_t *create_python_help(lv_obj_t *parent)
{
    if (!parent) return nullptr;

    lv_obj_t *overlay = lv_obj_create(parent);
    if (!overlay) return nullptr;
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, 320, 150);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(
        overlay, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    lv_obj_t *label = lv_label_create(overlay);
    if (!label) {
        lv_obj_delete(overlay);
        return nullptr;
    }
    lv_obj_set_pos(label, 12, 9);
    lv_obj_set_size(label, 296, 132);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, cp0_fonts().get_chrome(11), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_line_space(label, 2, 0);
    lv_label_set_text(label,
                      "python help\n"
                      "an interactive python repl.\n\n"
                      "fn + Z / C: move cursor left / right\n"
                      "fn + F / X: previous / next command\n"
                      "shift + fn + L: page up\n"
                      "shift + fn + M: page down");
    return overlay;
}

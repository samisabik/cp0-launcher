/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * ui_loading.cpp
 *
 * Transient "Loading..." overlay for app launches.
 *
 * Created lazily on first ui_loading::show() call as a child of
 * lv_layer_top() so it floats above any screen. Never deleted;
 * visibility is toggled via LV_OBJ_FLAG_HIDDEN. Three pixel blocks,
 * stepped by a timer only while the overlay is visible, sit to the left
 * of a short label inside a flat black panel with a 1 px border.
 *
 * Design notes:
 *   - Because internal page construction happens synchronously on the
 *     LVGL thread, callers should follow ui_loading::show() with
 *     lv_refr_now(NULL) to force the overlay to actually paint BEFORE
 *     the slow work begins. Otherwise LVGL would only render on the
 *     next frame, which is after the freeze.
 *   - For external (forked) apps, lv_refr_now() is already performed
 *     by launch_Exec() before the cp0 process callback - the
 *     overlay stays on-screen while the child owns the framebuffer,
 *     which is exactly the desired "something is happening" feedback.
 */

#include "ui_loading.h"
#include "ui.h"
#include "lvgl/lvgl.h"
#include "model/global_overlay_model.hpp"

#include <cctype>
#include <string>

#define LOADING_BG_COLOR    0x000000
#define LOADING_BORDER      0x1E2B23
#define LOADING_TEXT_COLOR  0xE6E6E6
#define LOADING_DOT_ON      0x3DDC84
#define LOADING_DOT_OFF     0x13261B
#define LOADING_WIDTH       200
#define LOADING_HEIGHT      34
#define LOADING_DOT_SIZE    4
#define LOADING_DOT_GAP     3
#define LOADING_STEP_MS     150

static lv_obj_t *s_loading_obj     = NULL;
static lv_obj_t *s_loading_spinner = NULL;
static lv_obj_t *s_loading_label   = NULL;
static lv_timer_t *s_loading_timer = NULL;
static uint32_t s_loading_step     = 0;
static GlobalOverlayModel s_loading_state;

static void loading_stop_timer(void) noexcept
{
    if (s_loading_timer != NULL) lv_timer_delete(s_loading_timer);
    s_loading_timer = NULL;
}

static void loading_paint_dots(void) noexcept
{
    if (s_loading_spinner == NULL) return;
    const uint32_t count = lv_obj_get_child_count(s_loading_spinner);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t *dot = lv_obj_get_child(s_loading_spinner, static_cast<int32_t>(i));
        lv_obj_set_style_bg_color(
            dot, lv_color_hex(i == s_loading_step % count ? LOADING_DOT_ON : LOADING_DOT_OFF), 0);
    }
}

static void loading_timer_cb(lv_timer_t *) noexcept
{
    ++s_loading_step;
    loading_paint_dots();
}

static void loading_deleted_cb(lv_event_t *event) noexcept
{
    try {
        if (lv_event_get_code(event) != LV_EVENT_DELETE ||
            lv_event_get_target(event) != lv_event_get_current_target(event) ||
            lv_event_get_target(event) != s_loading_obj)
            return;
        loading_stop_timer();
        s_loading_obj = NULL;
        s_loading_spinner = NULL;
        s_loading_label = NULL;
        s_loading_state.object_deleted();
    } catch (...) {
        loading_stop_timer();
        s_loading_obj = NULL;
        s_loading_spinner = NULL;
        s_loading_label = NULL;
        s_loading_state.object_deleted();
    }
}

static void loading_child_deleted_cb(lv_event_t *event) noexcept
{
    try {
        if (lv_event_get_code(event) != LV_EVENT_DELETE ||
            lv_event_get_target(event) != lv_event_get_current_target(event)) return;
        lv_obj_t *deleted = static_cast<lv_obj_t *>(lv_event_get_target(event));
        if (deleted != s_loading_spinner && deleted != s_loading_label) return;
        if (deleted == s_loading_spinner) s_loading_spinner = NULL;
        if (deleted == s_loading_label) s_loading_label = NULL;
        if (s_loading_obj != NULL)
            lv_obj_add_flag(s_loading_obj, LV_OBJ_FLAG_HIDDEN);
        s_loading_state.object_deleted();
    } catch (...) {
        s_loading_spinner = NULL;
        s_loading_label = NULL;
        s_loading_state.object_deleted();
    }
}

static void ensure_loading_created(void) noexcept
{
    try {
    if (s_loading_obj != NULL && s_loading_spinner != NULL &&
        s_loading_label != NULL) return;
    if (s_loading_obj != NULL) lv_obj_delete(s_loading_obj);

    lv_obj_t *parent = lv_layer_top();
    if (parent == NULL) return;

    s_loading_obj = lv_obj_create(parent);
    if (s_loading_obj == NULL) return;
    lv_obj_add_event_cb(s_loading_obj, loading_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_remove_style_all(s_loading_obj);
    lv_obj_set_size(s_loading_obj, LOADING_WIDTH, LOADING_HEIGHT);
    lv_obj_center(s_loading_obj);

    lv_obj_set_style_bg_color(s_loading_obj, lv_color_hex(LOADING_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_loading_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_loading_obj, 0, 0);
    lv_obj_set_style_border_width(s_loading_obj, 1, 0);
    lv_obj_set_style_border_color(s_loading_obj, lv_color_hex(LOADING_BORDER), 0);
    lv_obj_set_style_pad_all(s_loading_obj, 6, 0);
    lv_obj_set_style_shadow_width(s_loading_obj, 0, 0);

    /* Purely visual — never steal focus or clicks. */
    lv_obj_clear_flag(s_loading_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_loading_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_loading_obj, LV_OBJ_FLAG_IGNORE_LAYOUT);

    /* Horizontal layout: blocks on the left, label to its right. */
    lv_obj_set_flex_flow(s_loading_obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_loading_obj,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_loading_obj, 8, 0);

    s_loading_spinner = lv_obj_create(s_loading_obj);
    if (s_loading_spinner == NULL) {
        lv_obj_delete(s_loading_obj);
        return;
    }
    lv_obj_add_event_cb(
        s_loading_spinner, loading_child_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_remove_style_all(s_loading_spinner);
    lv_obj_set_size(s_loading_spinner, 3 * LOADING_DOT_SIZE + 2 * LOADING_DOT_GAP, LOADING_DOT_SIZE);
    lv_obj_clear_flag(s_loading_spinner, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *dot = lv_obj_create(s_loading_spinner);
        if (dot == NULL) continue;
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, LOADING_DOT_SIZE, LOADING_DOT_SIZE);
        lv_obj_set_pos(dot, i * (LOADING_DOT_SIZE + LOADING_DOT_GAP), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(LOADING_DOT_OFF), 0);
    }

    s_loading_label = lv_label_create(s_loading_obj);
    if (s_loading_label == NULL) {
        lv_obj_delete(s_loading_obj);
        return;
    }
    lv_obj_add_event_cb(
        s_loading_label, loading_child_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_text_color(s_loading_label,
                                lv_color_hex(LOADING_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(s_loading_label, launcher_fonts().get_chrome(11), 0);
    lv_label_set_text(s_loading_label, "");

    lv_obj_add_flag(s_loading_obj, LV_OBJ_FLAG_HIDDEN);
    s_loading_state.mark_created();
    } catch (...) {
        if (s_loading_obj != NULL)
            lv_obj_delete(s_loading_obj);
        s_loading_obj = NULL;
        s_loading_spinner = NULL;
        s_loading_label = NULL;
        s_loading_state.object_deleted();
    }
}

namespace ui_loading {

void show(const char *label)
{
    try {
    if (s_loading_state.request_show())
        ensure_loading_created();
    if (s_loading_obj == NULL || s_loading_label == NULL) return;

    std::string text = (label && label[0]) ? label : "loading";
    for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (text.size() > 3 && text.compare(text.size() - 3, 3, "...") == 0) text.resize(text.size() - 3);
    lv_label_set_text(s_loading_label, text.c_str());
    s_loading_step = 0;
    loading_paint_dots();
    if (s_loading_timer == NULL)
        s_loading_timer = lv_timer_create(loading_timer_cb, LOADING_STEP_MS, NULL);
    /* Make sure the overlay stays on top of any other lv_layer_top
     * children (e.g. the ui_global_hint toast). */
    lv_obj_move_foreground(s_loading_obj);
    lv_obj_clear_flag(s_loading_obj, LV_OBJ_FLAG_HIDDEN);
    } catch (...) {
        if (s_loading_obj != NULL)
            lv_obj_add_flag(s_loading_obj, LV_OBJ_FLAG_HIDDEN);
        s_loading_state.hide();
    }
}

void hide()
{
    loading_stop_timer();
    s_loading_state.hide();
    if (s_loading_obj == NULL) return;
    lv_obj_add_flag(s_loading_obj, LV_OBJ_FLAG_HIDDEN);
}

void shutdown()
{
    loading_stop_timer();
    s_loading_state.hide();
    if (s_loading_obj != NULL)
        lv_obj_delete(s_loading_obj);
    s_loading_obj = NULL;
    s_loading_spinner = NULL;
    s_loading_label = NULL;
    s_loading_state.object_deleted();
}

} // namespace ui_loading

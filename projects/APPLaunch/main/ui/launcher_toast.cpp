/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "launcher_toast.h"

#include "ui.h"

#include <cstring>

namespace {

constexpr uint32_t kShowDurationMs = 1500;
constexpr uint32_t kBackgroundColor = cp0::theme::kSelect;
constexpr uint32_t kTextColor = cp0::theme::kText;
constexpr lv_coord_t kWidth = 240;
constexpr lv_coord_t kHeight = 22;
constexpr lv_coord_t kMultilineHeight = 38;
constexpr lv_coord_t kProgressHeight = 40;
constexpr lv_coord_t kTopOffset = 4;

} // namespace

bool LauncherToast::ensure_created() noexcept
{
    try {
    if (container_ && label_) return true;
    if (container_) lv_obj_delete(container_);

    lv_obj_t *parent = lv_layer_top();
    if (!parent)
        return false;

    container_ = lv_obj_create(parent);
    if (!container_)
        return false;
    lv_obj_add_event_cb(container_, container_delete_cb, LV_EVENT_DELETE, this);
    lv_obj_remove_style_all(container_);
    lv_obj_set_size(container_, kWidth, kHeight);
    lv_obj_align(container_, LV_ALIGN_TOP_MID, 0, kTopOffset);
    lv_obj_set_style_bg_color(container_, lv_color_hex(kBackgroundColor), 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    cp0::ui::brackets(container_, cp0::theme::kAccent);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_shadow_width(container_, 0, 0);
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(container_, LV_OBJ_FLAG_IGNORE_LAYOUT);

    label_ = lv_label_create(container_);
    if (!label_) {
        lv_obj_delete(container_);
        return false;
    }
    lv_obj_add_event_cb(label_, label_delete_cb, LV_EVENT_DELETE, this);
    lv_obj_set_style_text_color(label_, lv_color_hex(kTextColor), 0);
    lv_obj_set_style_text_font(
        label_, launcher_fonts().get_chrome(11), 0);
    lv_obj_set_width(label_, kWidth - 12);
    lv_label_set_long_mode(label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label_);
    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
    return true;
    } catch (...) {
        if (container_) lv_obj_delete(container_);
        container_ = nullptr;
        label_ = nullptr;
        return false;
    }
}

void LauncherToast::show(const char *text) noexcept
{
    try {
    if (!ensure_created())
        return;

    lv_label_set_text(label_, text ? text : "");
    lv_obj_set_height(container_, text && std::strchr(text, '\n') ? kMultilineHeight : kHeight);
    lv_obj_center(label_);
    if (progress_.track) lv_obj_add_flag(progress_.track, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(container_, LV_ALIGN_TOP_MID, 0, kTopOffset);
    lv_obj_move_foreground(container_);
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
    if (!hide_timer_)
        hide_timer_ = lv_timer_create(hide_timer_cb, kShowDurationMs, this);
    if (!hide_timer_) {
        lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_timer_set_period(hide_timer_, kShowDurationMs);
    lv_timer_reset(hide_timer_);
    lv_timer_resume(hide_timer_);
    } catch (...) {
        hide();
    }
}

void LauncherToast::show_persistent(const char *text, bool centered) noexcept
{
    show(text);
    if (centered && container_) lv_obj_align(container_, LV_ALIGN_CENTER, 0, 0);
    if (hide_timer_) lv_timer_pause(hide_timer_);
}

void LauncherToast::set_progress(float fraction) noexcept
{
    try {
        if (!container_ || !label_) return;
        if (!progress_.track) progress_ = cp0::ui::segbar(container_, 12, 26, kWidth - 24, 7, kBackgroundColor);
        if (!progress_.track) return;
        lv_obj_set_height(container_, kProgressHeight);
        lv_obj_align(label_, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_clear_flag(progress_.track, LV_OBJ_FLAG_HIDDEN);
        cp0::ui::set_segbar(progress_, fraction, cp0::theme::kAccent);
    } catch (...) {
    }
}

void LauncherToast::hide()
{
    if (container_)
        lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
    if (hide_timer_)
        lv_timer_pause(hide_timer_);
}

void LauncherToast::shutdown()
{
    if (container_)
        lv_obj_delete(container_);
    else if (hide_timer_) {
        lv_timer_delete(hide_timer_);
        hide_timer_ = nullptr;
    }
    container_ = nullptr;
    label_ = nullptr;
    progress_ = {};
}

void LauncherToast::hide_timer_cb(lv_timer_t *timer) noexcept
{
    try {
    auto *toast = static_cast<LauncherToast *>(lv_timer_get_user_data(timer));
    if (toast && toast->hide_timer_ == timer)
        toast->hide();
    } catch (...) {
    }
}

void LauncherToast::container_delete_cb(lv_event_t *event) noexcept
{
    try {
        auto *toast = static_cast<LauncherToast *>(lv_event_get_user_data(event));
        if (!toast || lv_event_get_code(event) != LV_EVENT_DELETE ||
            lv_event_get_target(event) != lv_event_get_current_target(event) ||
            lv_event_get_target(event) != toast->container_)
            return;
        toast->container_ = nullptr;
        toast->label_ = nullptr;
        toast->progress_ = {};
        if (toast->hide_timer_) {
            lv_timer_delete(toast->hide_timer_);
            toast->hide_timer_ = nullptr;
        }
    } catch (...) {
    }
}

void LauncherToast::label_delete_cb(lv_event_t *event) noexcept
{
    try {
        if (lv_event_get_code(event) != LV_EVENT_DELETE ||
            lv_event_get_target(event) != lv_event_get_current_target(event)) return;
        auto *toast = static_cast<LauncherToast *>(lv_event_get_user_data(event));
        if (toast && lv_event_get_target(event) == toast->label_) {
            toast->label_ = nullptr;
            toast->hide();
        }
    } catch (...) {
    }
}

LauncherToast &launcher_toast()
{
    static LauncherToast toast;
    return toast;
}

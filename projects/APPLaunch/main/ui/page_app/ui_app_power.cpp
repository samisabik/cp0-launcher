/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_power.hpp"

#include "cp0_lvgl_app.h"
#include "input_keys.h"

#include <string>
#include <tuple>

namespace {

constexpr uint32_t kColorAccent = 0x3DDC84;
constexpr uint32_t kColorText = 0xE6E6E6;
constexpr uint32_t kColorIdle = 0x8A8A8A;
constexpr uint32_t kColorDim = 0x5F7A66;
constexpr int kFooterY = 135;
// Long enough to read the instructions before the backlight goes off.
constexpr uint32_t kShutdownDelayMs = 1500;

lv_obj_t *label(lv_obj_t *parent, int x, int y, int px, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    if (!obj) return nullptr;
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_text_font(obj, launcher_fonts().get_chrome(px), 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_label_set_text(obj, "");
    return obj;
}

int text_width(lv_obj_t *obj)
{
    lv_obj_update_layout(obj);
    return lv_obj_get_width(obj);
}

} // namespace

UIPowerPage::UIPowerPage() : AppPage()
{
    set_page_title("Power");
    create_ui();
    render();
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

UIPowerPage::~UIPowerPage()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
}

void UIPowerPage::create_ui()
{
    if (!ui_APP_Container) return;
    lv_obj_set_style_bg_color(ui_APP_Container, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_APP_Container, LV_OPA_COVER, 0);
    title_ = label(ui_APP_Container, 16, 30, 22, kColorText);
    detail_ = label(ui_APP_Container, 16, 66, 11, kColorIdle);
    if (detail_) {
        lv_obj_set_width(detail_, 288);
        lv_label_set_long_mode(detail_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_line_space(detail_, 3, 0);
    }
    key_ = label(ui_APP_Container, 4, kFooterY, 11, kColorAccent);
    word_ = label(ui_APP_Container, 0, kFooterY, 11, kColorDim);
    key2_ = label(ui_APP_Container, 0, kFooterY, 11, kColorAccent);
    word2_ = label(ui_APP_Container, 0, kFooterY, 11, kColorDim);
}

void UIPowerPage::render()
{
    const char *title = "power off?";
    const char *detail = "shuts linux down cleanly so the sd card is safe. flip the power switch afterwards.";
    const char *key = "ent";
    const char *word = " confirm";
    const char *key2 = "esc";
    const char *word2 = " cancel";
    if (state_ == State::Confirm) {
        title = "sure?";
        detail = "unsaved work in open apps will be lost.";
        word = " power off";
    } else if (state_ == State::ShuttingDown) {
        title = "powering off";
        detail = "the screen goes dark now. wait about 10 s, then flip the switch.";
        key = word = key2 = word2 = "";
    } else if (state_ == State::Refused) {
        title = "power off refused";
        detail = "linux is still running, do not flip the switch. the system did not allow the shutdown.";
        word = " try again";
        word2 = " back";
    }
    if (title_) {
        lv_label_set_text(title_, title);
        const uint32_t color = state_ == State::ShuttingDown ? kColorAccent
                               : state_ == State::Refused    ? 0xFF5F56
                                                             : kColorText;
        lv_obj_set_style_text_color(title_, lv_color_hex(color), 0);
    }
    if (detail_) lv_label_set_text(detail_, detail);
    int x = 4;
    for (auto [obj, text, gap] : {std::tuple<lv_obj_t *, const char *, int>{key_, key, 0},
                                  {word_, word, 14},
                                  {key2_, key2, 0},
                                  {word2_, word2, 0}}) {
        if (!obj) continue;
        lv_label_set_text(obj, text);
        lv_obj_set_x(obj, x);
        x += text_width(obj) + gap;
    }
}

void UIPowerPage::shutdown_cb(lv_timer_t *timer) noexcept
{
    auto *page = static_cast<UIPowerPage *>(lv_timer_get_user_data(timer));
    if (page) page->timer_ = nullptr;
    lv_timer_delete(timer);
    bool refused = true;
    try {
        cp0_signal_process_api({"Shutdown"}, [&refused](int code, std::string) { refused = code != 0; });
    } catch (...) {
    }
    if (refused && page) page->shutdown_refused();
}

void UIPowerPage::shutdown_refused()
{
    state_ = State::Refused;
    render();
}

void UIPowerPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UIPowerPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item || item->key_state == KBD_KEY_REPEATED) return;
    try {
        page->handle_key(item->semantic_key ? item->semantic_key : item->key_code);
    } catch (...) {
    }
}

void UIPowerPage::handle_key(uint32_t key)
{
    if (state_ == State::ShuttingDown) return;
    if (key == KEY_ESC) {
        if (state_ == State::Confirm || state_ == State::Refused) {
            state_ = State::Ask;
            render();
        } else if (navigate_home) {
            navigate_home();
        }
        return;
    }
    if (key != KEY_ENTER && key != KEY_KPENTER) return;
    if (state_ == State::Ask || state_ == State::Refused) {
        state_ = State::Confirm;
        render();
        return;
    }
    state_ = State::ShuttingDown;
    render();
    timer_ = lv_timer_create(shutdown_cb, kShutdownDelayMs, this);
}

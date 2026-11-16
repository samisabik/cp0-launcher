/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_power.hpp"

#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "input_keys.h"

#include <string>

namespace {

namespace theme = cp0::theme;

// Long enough to read the instructions before the backlight goes off.
constexpr uint32_t kShutdownDelayMs = 1500;

} // namespace

UIPowerPage::UIPowerPage() : AppPage()
{
    set_page_title("Power");
    reclaim_top_bar();
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
    cp0::ui::paint_background(ui_APP_Container);
    cp0::ui::title_row(ui_APP_Container, "power");
    title_ = cp0::ui::label(ui_APP_Container, 16, 32, theme::kText, true);
    detail_ = cp0::ui::label(ui_APP_Container, 16, 66, theme::kIdle);
    if (detail_) {
        lv_obj_set_width(detail_, 288);
        lv_label_set_long_mode(detail_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_line_space(detail_, 3, 0);
    }
    footer_ = cp0::ui::footer(ui_APP_Container);
}

void UIPowerPage::render()
{
    const char *title = "power off?";
    const char *detail = "shuts linux down cleanly so the sd card is safe. flip the power switch afterwards.";
    std::vector<cp0::ui::Chip> chips{{"ent", "confirm"}, {"esc", "cancel"}};
    if (state_ == State::Confirm) {
        title = "sure?";
        detail = "unsaved work in open apps will be lost.";
        chips = {{"ent", "power off"}, {"esc", "cancel"}};
    } else if (state_ == State::ShuttingDown) {
        title = "powering off";
        detail = "the screen goes dark now. wait about 10 s, then flip the switch.";
        chips.clear();
    } else if (state_ == State::Refused) {
        title = "power off refused";
        detail = "linux is still running, do not flip the switch. the system did not allow the shutdown.";
        chips = {{"ent", "try again"}, {"esc", "back"}};
    }
    if (title_) {
        lv_label_set_text(title_, title);
        const uint32_t color = state_ == State::ShuttingDown ? theme::kAccent
                               : state_ == State::Refused    ? theme::kDanger
                                                             : theme::kText;
        lv_obj_set_style_text_color(title_, lv_color_hex(color), 0);
    }
    if (detail_) lv_label_set_text(detail_, detail);
    cp0::ui::set_footer(footer_, chips);
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

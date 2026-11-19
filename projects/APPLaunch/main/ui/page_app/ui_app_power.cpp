/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT

#include "ui_app_power.hpp"

#include "../settings/settings_power_overlay.hpp"
#include "cp0_theme.hpp"
#include "input_keys.h"

#include <algorithm>
#include <string>

namespace {

namespace boot = settings_t12b::boot_actions;

constexpr boot::Action kActions[] = {boot::Action::Shutdown, boot::Action::Reboot};

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
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
}

void UIPowerPage::create_ui()
{
    if (!ui_APP_Container) return;
    cp0::ui::paint_background(ui_APP_Container);
    cp0::ui::title_row(ui_APP_Container, "power");

    for (int i = 0; i < kRows; ++i) {
        Row &row = rows_[i];
        row.panel = cp0::ui::panel(ui_APP_Container, 4, 30 + i * 40, 312, 34);
        if (!row.panel.box) continue;
        row.label = cp0::ui::label(row.panel.box, 12, 9, cp0::theme::kText, true);
        if (row.label) lv_label_set_text(row.label, std::string(boot::presentation(kActions[i]).label).c_str());
    }

    hint_ = cp0::ui::footer(ui_APP_Container);
}

void UIPowerPage::render()
{
    for (int i = 0; i < kRows; ++i) {
        const bool on = i == selected_;
        cp0::ui::set_panel_focus(rows_[i].panel, on);
        if (rows_[i].label)
            lv_obj_set_style_text_color(rows_[i].label, lv_color_hex(on ? cp0::theme::kAccent : cp0::theme::kText), 0);
    }
    cp0::ui::set_footer(hint_, {{"up/dn", "pick"}, {"ent", "run"}, {"esc", "home"}});
}

void UIPowerPage::activate()
{
    settings_power_request(kActions[selected_]);
}

void UIPowerPage::handle_key(uint32_t key)
{
    if (key == KEY_ESC) {
        if (navigate_home) navigate_home();
    } else if (key == KEY_UP || key == KEY_DOWN) {
        selected_ = (selected_ + (key == KEY_DOWN ? 1 : kRows - 1)) % kRows;
        render();
    } else if (key == KEY_ENTER) {
        activate();
    }
}

void UIPowerPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UIPowerPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item) return;
    const uint32_t key = item->semantic_key ? item->semantic_key : item->key_code;
    try {
        page->handle_key(key);
    } catch (...) {
    }
}

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_ip_panel.hpp"

void UIIpPanelPage::create_ui()
{
    if (!ui_APP_Container) return;
    lv_obj_t *background = lv_obj_create(ui_APP_Container);
    if (!background) return;
    lv_obj_set_size(background, 320, 150);
    lv_obj_set_pos(background, 0, 0);
    lv_obj_set_style_radius(background, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(background, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(background, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(background, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(background, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(background, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_bar = lv_obj_create(background);
    if (!title_bar) { lv_obj_delete(background); return; }
    lv_obj_set_size(title_bar, 320, TITLE_H);
    lv_obj_set_pos(title_bar, 0, 0);
    lv_obj_set_style_radius(title_bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(title_bar, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(title_bar, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(title_bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(title_bar, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(title_bar);
    if (!title) { lv_obj_delete(background); return; }
    lv_label_set_text(title, "ip panel");
    lv_obj_set_align(title, LV_ALIGN_LEFT_MID);
    lv_obj_set_style_text_color(title, lv_color_hex(0x3DDC84), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title, launcher_fonts().get_chrome(11), LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *hint = lv_label_create(title_bar);
    if (!hint) { lv_obj_delete(background); return; }
    lv_label_set_text(hint, "fn+h help");
    lv_obj_set_align(hint, LV_ALIGN_RIGHT_MID);
    lv_obj_set_x(hint, -4);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x5F7A66), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(hint, launcher_fonts().get_chrome(11), LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *list_container = lv_obj_create(background);
    if (!list_container) { lv_obj_delete(background); return; }
    background_ = background;
    list_container_ = list_container;
    lv_obj_add_event_cb(
        list_container_, static_owned_obj_delete_cb, LV_EVENT_DELETE, this);
    lv_obj_add_event_cb(
        background_, static_owned_obj_delete_cb, LV_EVENT_DELETE, this);
    lv_obj_set_size(list_container_, 320, LIST_H);
    lv_obj_set_pos(list_container_, 0, TITLE_H);
    lv_obj_set_style_radius(list_container_, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(list_container_, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(list_container_, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(list_container_, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(list_container_, LV_OBJ_FLAG_SCROLLABLE);

    (void)fetch_iface_list();
    build_list_rows();
    refresh_timer_.start(
        [this] { return lv_timer_create(UIIpPanelPage::static_timer_cb, 1000, this); },
        [](lv_timer_t *timer) { lv_timer_delete(timer); });
}

bool UIIpPanelPage::build_help_view()
{
    if (!background_ || help_container_) return help_container_ != nullptr;

    lv_obj_t *candidate = lv_obj_create(background_);
    if (!candidate) return false;
    lv_obj_set_size(candidate, 320, 150);
    lv_obj_set_pos(candidate, 0, 0);
    lv_obj_set_style_radius(candidate, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(candidate, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(candidate, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(candidate, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(candidate, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(candidate, LV_OBJ_FLAG_SCROLLABLE);

    auto add_label = [candidate](const char *text, int y, uint32_t color,
                                 const lv_font_t *font, bool wrap = false) {
        lv_obj_t *label = lv_label_create(candidate);
        if (!label) return false;
        lv_label_set_text(label, text);
        lv_obj_set_pos(label, 8, y);
        lv_obj_set_style_text_color(label, lv_color_hex(color),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(label, font, LV_PART_MAIN | LV_STATE_DEFAULT);
        if (wrap) {
            lv_obj_set_width(label, 304);
            lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        }
        return true;
    };

    const lv_font_t *font = launcher_fonts().get_chrome(11);
    if (!add_label("help", 6, 0x3DDC84, font) ||
        !add_label("shows the wi-fi and ethernet ip addresses of this device.", 31, 0xE6E6E6, font, true) ||
        !add_label("esc back", 135, 0x5F7A66, font)) {
        lv_obj_delete(candidate);
        return false;
    }

    lv_obj_add_event_cb(candidate, static_owned_obj_delete_cb, LV_EVENT_DELETE, this);
    help_container_ = candidate;
    lv_obj_move_foreground(help_container_);
    return true;
}

void UIIpPanelPage::show_help()
{
    if (view_state_ != ViewState::MAIN) return;
    if (build_help_view()) view_state_ = ViewState::HELP;
}

void UIIpPanelPage::close_help()
{
    if (view_state_ != ViewState::HELP) return;
    if (help_container_) lv_obj_delete(help_container_);
    view_state_ = ViewState::MAIN;
}

void UIIpPanelPage::build_list_rows()
{
    if (!list_container_) return;
    lv_obj_clean(list_container_);
    const auto &interfaces = model_.interfaces();
    if (interfaces.empty()) {
        lv_obj_t *label = lv_label_create(list_container_);
        if (!label) return;
        lv_label_set_text(label, "no network interface found");
        lv_obj_set_pos(label, 10, 50);
        lv_obj_set_style_text_color(label, lv_color_hex(0x5C5C5C), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(label, launcher_fonts().get_chrome(11), LV_PART_MAIN | LV_STATE_DEFAULT);
        return;
    }

    const size_t offset = model_.visible_offset(VISIBLE_ROWS);
    for (size_t row = 0; row < VISIBLE_ROWS && row + offset < interfaces.size(); ++row) {
        const size_t index = row + offset;
        create_row(list_container_, static_cast<int>(row), index,
                   index == model_.selected_index());
    }
}

void UIIpPanelPage::create_row(lv_obj_t *parent, int visual_row, size_t index, bool selected)
{
    const auto &interfaces = model_.interfaces();
    if (!parent || index >= interfaces.size()) return;
    const IpInterfaceInfo &info = interfaces[index];
    lv_obj_t *row = lv_obj_create(parent);
    if (!row) return;
    lv_obj_set_size(row, 318, ITEM_H - 2);
    lv_obj_set_pos(row, 1, visual_row * ITEM_H + 1);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(selected ? 0x13261B : 0x000000),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(row, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    if (index + 1 < interfaces.size()) {
        lv_obj_t *divider = lv_obj_create(parent);
        if (divider) {
        lv_obj_set_size(divider, 310, 1);
        lv_obj_set_pos(divider, 5, (visual_row + 1) * ITEM_H);
        lv_obj_set_style_radius(divider, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(divider, lv_color_hex(0x1E2B23), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(divider, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(divider, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    lv_obj_t *status_dot = lv_obj_create(row);
    if (status_dot) {
    lv_obj_set_size(status_dot, 6, 6);
    lv_obj_set_pos(status_dot, 8, (ITEM_H - 8) / 2);
    lv_obj_set_style_radius(status_dot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(status_dot, lv_color_hex(info.up ? 0x3DDC84 : 0x3C3C3C),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(status_dot, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(status_dot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(status_dot, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_t *name = lv_label_create(row);
    if (name) {
    lv_label_set_text(name, info.name.c_str());
    lv_obj_set_pos(name, 20, 2);
    lv_obj_set_width(name, 72);
    lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(name, lv_color_hex(selected ? 0x3DDC84 : 0x8A8A8A),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(name, launcher_fonts().get_chrome(11), LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    lv_obj_t *address = lv_label_create(row);
    if (address) {
    lv_label_set_text(address, info.address.c_str());
    lv_obj_set_pos(address, 96, 2);
    lv_obj_set_width(address, 130);
    lv_label_set_long_mode(address, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(address, lv_color_hex(selected ? 0xE6E6E6 : 0x8A8A8A),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(address, launcher_fonts().get_chrome(11), LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    lv_obj_t *netmask = lv_label_create(row);
    if (netmask) {
    lv_label_set_text(netmask, info.netmask.c_str());
    lv_obj_set_pos(netmask, 96, 17);
    lv_obj_set_width(netmask, 130);
    lv_label_set_long_mode(netmask, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(netmask, lv_color_hex(0x5F7A66), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(netmask, launcher_fonts().get_chrome(11), LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    lv_obj_t *state = lv_label_create(row);
    if (state) {
    lv_label_set_text(state, info.up ? "up" : "down");
    lv_obj_set_pos(state, 266, 2);
    lv_obj_set_style_text_color(state, lv_color_hex(info.up ? 0x3DDC84 : 0x5C5C5C),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(state, launcher_fonts().get_chrome(11), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_ip_panel.hpp"

#include "cp0_theme.hpp"

#include <cstdio>
#include <string>

namespace theme = cp0::theme;

void UIIpPanelPage::create_ui()
{
    if (!ui_APP_Container) return;
    lv_obj_t *background = cp0::ui::box(ui_APP_Container, 0, 0, theme::kScreenW, theme::kScreenH, theme::kBg);
    if (!background) return;

    const cp0::ui::TitleRow title = cp0::ui::title_row(background, "ip panel");
    status_label_ = title.status;
    cp0::ui::set_footer(cp0::ui::footer(background), {{"up/dn", "move"}, {"fn+h", "help"}});

    lv_obj_t *list_container = lv_obj_create(background);
    if (!list_container) { lv_obj_delete(background); return; }
    background_ = background;
    list_container_ = list_container;
    lv_obj_add_event_cb(
        list_container_, static_owned_obj_delete_cb, LV_EVENT_DELETE, this);
    lv_obj_add_event_cb(
        background_, static_owned_obj_delete_cb, LV_EVENT_DELETE, this);
    lv_obj_set_size(list_container_, 320, LIST_H);
    lv_obj_set_pos(list_container_, 0, theme::kBodyTop);
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

    lv_obj_t *candidate = cp0::ui::box(background_, 0, 0, theme::kScreenW, theme::kScreenH, theme::kBg);
    if (!candidate) return false;
    cp0::ui::title_row(candidate, "ip panel help");
    lv_obj_t *body = cp0::ui::label(candidate, theme::kPadX, 22, theme::kText);
    if (body) {
        lv_obj_set_width(body, theme::kScreenW - 2 * theme::kPadX);
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_label_set_text(body, "shows the wi-fi, ethernet and vpn addresses of this device.");
    }
    lv_obj_t *footer = cp0::ui::footer(candidate);
    cp0::ui::set_footer(footer, {{"esc", "close"}});
    if (!body || !footer) {
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
    std::size_t up = 0;
    for (const IpInterfaceInfo &info : interfaces) up += info.up ? 1 : 0;
    cp0::ui::set_text(status_label_, (std::to_string(up) + " up").c_str(), theme::kIdle);
    if (interfaces.empty()) {
        lv_obj_t *label = lv_label_create(list_container_);
        if (!label) return;
        lv_label_set_text(label, "no network interface found");
        lv_obj_set_pos(label, 10, 50);
        lv_obj_set_style_text_color(label, lv_color_hex(theme::kDim), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(label, cp0::ui::font(), LV_PART_MAIN | LV_STATE_DEFAULT);
        return;
    }

    const size_t offset = model_.visible_offset(VISIBLE_ROWS);
    for (size_t row = 0; row < VISIBLE_ROWS && row + offset < interfaces.size(); ++row) {
        const size_t index = row + offset;
        create_row(list_container_, static_cast<int>(row), index,
                   index == model_.selected_index());
    }
}

namespace {

// "255.255.255.0" -> "/24"; empty when the mask is not a dotted quad.
std::string cidr_suffix(const std::string &mask)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(mask.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return "";
    const uint32_t bits = (a << 24) | (b << 16) | (c << 8) | d;
    int count = 0;
    for (uint32_t v = bits; v & 0x80000000u; v <<= 1) ++count;
    return "/" + std::to_string(count);
}

} // namespace

void UIIpPanelPage::create_row(lv_obj_t *parent, int visual_row, size_t index, bool selected)
{
    const auto &interfaces = model_.interfaces();
    if (!parent || index >= interfaces.size()) return;
    const IpInterfaceInfo &info = interfaces[index];
    const cp0::ui::Panel row = cp0::ui::panel(parent, 4, visual_row * ITEM_H + 2, 312, ITEM_H - 4);
    if (!row.box) return;
    cp0::ui::set_panel_focus(row, selected);
    const int text_y = (ITEM_H - 4 - 13) / 2;

    if (lv_obj_t *name = cp0::ui::label(row.box, 6, text_y, selected ? theme::kAccent : theme::kText)) {
        lv_obj_set_width(name, 86);
        lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
        lv_label_set_text(name, info.name.c_str());
    }
    const std::string cidr = cidr_suffix(info.netmask);
    if (lv_obj_t *address = cp0::ui::label(row.box, 96, text_y, theme::kText)) {
        lv_label_set_recolor(address, true);
        char suffix[32] = "";
        if (!cidr.empty()) std::snprintf(suffix, sizeof(suffix), "#%06X %s#", static_cast<unsigned>(theme::kIdle), cidr.c_str());
        lv_label_set_text(address, (info.address + suffix).c_str());
    }
    lv_obj_t *badge = cp0::ui::box(row.box, 312 - 6 - 24, text_y, 24, 13, info.up ? theme::kAccent : theme::kTrack);
    if (lv_obj_t *state = badge ? cp0::ui::label(badge, 0, 0, info.up ? theme::kBg : theme::kIdle) : nullptr) {
        lv_obj_set_width(state, 24);
        lv_obj_set_style_text_align(state, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(state, info.up ? "up" : "dn");
    }
}

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_ssh.hpp"
#include "../model/ssh_view_build_contract.hpp"

#include "cp0_theme.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace {

static const uint32_t &kColorAccent = cp0::theme::kAccent;
static const uint32_t &kColorText = cp0::theme::kText;
static const uint32_t &kColorIdle = cp0::theme::kIdle;
static const uint32_t &kColorDim = cp0::theme::kDim;
static const uint32_t &kColorError = cp0::theme::kDanger;
constexpr int kScreenH = cp0::theme::kScreenH;
constexpr int kValueX = 52;

lv_obj_t *plain_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, bool filled)
{
    lv_obj_t *box = lv_obj_create(parent);
    if (!box) return nullptr;
    lv_obj_remove_style_all(box);
    lv_obj_clear_flag(box, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_color(box, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(box, filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    return box;
}

lv_obj_t *text(lv_obj_t *parent, const std::string &value, int x, int y, uint32_t color, int px)
{
    lv_obj_t *label = lv_label_create(parent);
    if (!label) return nullptr;
    lv_label_set_text(label, value.c_str());
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, launcher_fonts().get_chrome(px), 0);
    return label;
}

int text_width(const std::string &value, int px)
{
    lv_point_t size{};
    lv_text_get_size(&size, value.c_str(), launcher_fonts().get_chrome(px), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

bool chips(lv_obj_t *parent, const std::vector<cp0::ui::Chip> &items)
{
    lv_obj_t *footer = cp0::ui::footer(parent);
    cp0::ui::set_footer(footer, items);
    return footer != nullptr;
}

std::string lower(const char *value)
{
    std::string out = value ? value : "";
    for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

void UISSHPage::create_ui()
{
    if (!ui_APP_Container) return;
    background_ = lv_obj_create(ui_APP_Container);
    if (!background_) return;
    lv_obj_add_event_cb(background_, static_owned_obj_delete_cb, LV_EVENT_DELETE, this);
    lv_obj_remove_style_all(background_);
    lv_obj_set_size(background_, 320, kScreenH);
    lv_obj_set_pos(background_, 0, 0);
    lv_obj_set_style_bg_color(background_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(background_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(background_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    if (!build_input_fields()) {
        lv_obj_delete(background_);
        background_ = nullptr;
        form_container_ = nullptr;
    }
}

bool UISSHPage::build_help_view()
{
    if (!background_ || help_container_) return help_container_ != nullptr;

    lv_obj_t *candidate = plain_box(background_, 0, 0, 320, kScreenH, 0x000000, true);
    if (!candidate) return false;

    struct Line {
        const char *key;
        const char *action;
    };
    static constexpr Line kLines[] = {
        {"up/dn", "move between host, port and user"},
        {"type", "edit the selected field"},
        {"enter", "connect"},
        {"tab", "save these details for next time"},
        {"esc", "back"},
    };
    const cp0::ui::TitleRow title = cp0::ui::title_row(candidate, "ssh help");
    bool ok = title.rule && text(candidate, "connect to another machine over ssh", 4, 22, kColorText, 11);
    int y = 44;
    for (const Line &line : kLines) {
        ok = ok && text(candidate, line.key, 4, y, kColorAccent, 11) &&
             text(candidate, line.action, 60, y, kColorIdle, 11);
        y += 15;
    }
    ok = ok && chips(candidate, {{"esc", "close"}});
    if (!ok) {
        lv_obj_delete(candidate);
        return false;
    }

    lv_obj_add_event_cb(candidate, static_owned_obj_delete_cb, LV_EVENT_DELETE, this);
    help_container_ = candidate;
    lv_obj_move_foreground(help_container_);
    return true;
}

void UISSHPage::show_help()
{
    if (view_state_ != ViewState::INPUT) return;
    if (build_help_view()) {
        restore_text_input_mode();
        view_state_ = ViewState::HELP;
    }
}

void UISSHPage::close_help()
{
    if (view_state_ != ViewState::HELP) return;
    if (help_container_) lv_obj_delete(help_container_);
    view_state_ = ViewState::INPUT;
    enter_text_input_mode();
}

// The form is rebuilt on every edit: a command preview, one row per field
// (the active one at 22 px with a cursor) and a footer of key hints.
bool UISSHPage::build_input_fields()
{
    if (!background_) return false;
    lv_obj_t *candidate = plain_box(background_, 0, 0, 320, kScreenH, 0x000000, false);
    if (!candidate) return false;
    try {
        bool ok = true;
        const std::string host = model_.value(0);
        const std::string port = model_.value(1);
        const std::string user = model_.value(2);
        std::string target;
        if (!user.empty()) target += user + "@";
        target += host.empty() ? "host" : host;
        if (!port.empty() && port != "22") target += ":" + port;
        const cp0::ui::TitleRow title = cp0::ui::title_row(candidate, "ssh");
        cp0::ui::set_text(title.status, target.c_str(), host.empty() ? kColorDim : kColorIdle);
        ok = ok && title.rule != nullptr;

        SshViewBuildContract build(SshConnectionModel::FIELD_COUNT);
        int y = 19;
        for (size_t index = 0; ok && index < SshConnectionModel::FIELD_COUNT; ++index) {
            const bool active = index == model_.active_index();
            const int row_h = active ? 40 : 20;
            const cp0::ui::Panel field = cp0::ui::panel(candidate, 4, y, 312, row_h);
            if (!field.box) {
                ok = false;
                break;
            }
            cp0::ui::set_panel_focus(field, active);
            ok = ok && text(field.box, lower(model_.label(index)), 6, active ? 2 : 4,
                            active ? kColorAccent : kColorIdle, 11);
            std::string value = model_.value(index);
            const bool empty = value.empty();
            if (index == 1 && empty) value = "22";
            const int value_x = active ? 6 : kValueX - 4;
            lv_obj_t *value_label = text(field.box, value, value_x, active ? 14 : 4,
                                         active ? kColorText : (empty ? kColorDim : kColorText), active ? 22 : 11);
            if (!value_label) {
                ok = false;
                break;
            }
            lv_obj_set_width(value_label, 312 - value_x - 16);
            lv_label_set_long_mode(value_label, LV_LABEL_LONG_CLIP);
            if (active) {
                const int cursor_x = value_x + std::min(text_width(model_.value(index), 22), 312 - value_x - 20);
                if (!plain_box(field.box, cursor_x + 1, 14 + 6, 9, 16, kColorAccent, true)) {
                    ok = false;
                    break;
                }
            }
            build.row_completed();
            y += row_h + 4;
        }

        if (ok && !status_message_.empty()) {
            lv_obj_t *status = cp0::ui::footer(candidate);
            cp0::ui::set_footer_message(status, lower(status_message_.c_str()).c_str(),
                                        status_error_ ? kColorError : kColorAccent);
            ok = status != nullptr;
        } else if (ok) {
            ok = chips(candidate, {{"ent", "connect"}, {"tab", "save"}, {"f1", "help"}});
        }
        if (ok) build.hint_completed();
        if (!ok || !build.ready()) {
            lv_obj_delete(candidate);
            return false;
        }

        lv_obj_add_event_cb(candidate, static_owned_obj_delete_cb, LV_EVENT_DELETE, this);
        lv_obj_t *old = form_container_;
        form_container_ = candidate;
        if (old) lv_obj_delete(old);
        if (help_container_) lv_obj_move_foreground(help_container_);
        return true;
    } catch (...) {
        lv_obj_delete(candidate);
        return false;
    }
}

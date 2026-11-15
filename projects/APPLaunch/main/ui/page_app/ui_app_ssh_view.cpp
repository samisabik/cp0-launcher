/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_ssh.hpp"
#include "../model/ssh_view_build_contract.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace {

constexpr uint32_t kColorAccent = 0x3DDC84;
constexpr uint32_t kColorText = 0xE6E6E6;
constexpr uint32_t kColorIdle = 0x8A8A8A;
constexpr uint32_t kColorDim = 0x5F7A66;
constexpr uint32_t kColorSelect = 0x13261B;
constexpr uint32_t kColorLine = 0x1E2B23;
constexpr uint32_t kColorError = 0xFF5F56;
constexpr int kFooterY = 135;
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

// Key in accent, action in dim, as in the other launcher pages.
bool chips(lv_obj_t *parent, std::initializer_list<std::pair<const char *, const char *>> items, int y)
{
    int x = 4;
    for (const auto &item : items) {
        const std::string key = item.first;
        const std::string word = std::string(" ") + item.second;
        if (!text(parent, key, x, y, kColorAccent, 11)) return false;
        x += text_width(key, 11);
        if (!text(parent, word, x, y, kColorDim, 11)) return false;
        x += text_width(word, 11) + 14;
    }
    return true;
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
    lv_obj_set_size(background_, 320, 150);
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

    lv_obj_t *candidate = plain_box(background_, 0, 0, 320, 150, 0x000000, true);
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
    bool ok = text(candidate, "$ man ssh", 4, 1, kColorDim, 11) &&
              text(candidate, "connect to another machine over ssh", 4, 18, kColorText, 11);
    int y = 42;
    for (const Line &line : kLines) {
        ok = ok && text(candidate, line.key, 4, y, kColorAccent, 11) &&
             text(candidate, line.action, 60, y, kColorIdle, 11);
        y += 15;
    }
    ok = ok && chips(candidate, {{"esc", "close"}}, kFooterY);
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
    lv_obj_t *candidate = plain_box(background_, 0, 0, 320, 150, 0x000000, false);
    if (!candidate) return false;
    try {
        bool ok = true;
        const std::string host = model_.value(0);
        const std::string port = model_.value(1);
        const std::string user = model_.value(2);
        std::string command = "ssh ";
        if (!user.empty()) command += user + "@";
        command += host.empty() ? "host" : host;
        if (!port.empty() && port != "22") command += " -p " + port;
        ok = ok && text(candidate, "$", 4, 1, kColorDim, 11);
        if (lv_obj_t *preview = text(candidate, command, 18, 1, host.empty() ? kColorDim : kColorAccent, 11)) {
            lv_obj_set_width(preview, 298);
            lv_label_set_long_mode(preview, LV_LABEL_LONG_CLIP);
        } else {
            ok = false;
        }
        ok = ok && plain_box(candidate, 0, 16, 320, 1, kColorLine, true) != nullptr;

        SshViewBuildContract build(SshConnectionModel::FIELD_COUNT);
        int y = 24;
        for (size_t index = 0; ok && index < SshConnectionModel::FIELD_COUNT; ++index) {
            const bool active = index == model_.active_index();
            const int row_h = active ? 30 : 20;
            if (active) {
                lv_obj_t *bar = plain_box(candidate, 2, y, 316, row_h, kColorSelect, true);
                if (!bar) {
                    ok = false;
                    break;
                }
            }
            const int value_px = active ? 22 : 11;
            const int value_h = active ? 24 : 13;
            ok = ok && text(candidate, lower(model_.label(index)), 10, y + (row_h - 13) / 2,
                            active ? kColorAccent : kColorDim, 11);
            std::string value = model_.value(index);
            const bool empty = value.empty();
            if (index == 1 && empty) value = "22";
            lv_obj_t *value_label = text(candidate, value, kValueX, y + (row_h - value_h) / 2,
                                         active ? kColorText : (empty ? kColorDim : kColorIdle), value_px);
            if (!value_label) {
                ok = false;
                break;
            }
            lv_obj_set_width(value_label, 320 - kValueX - 14);
            lv_label_set_long_mode(value_label, LV_LABEL_LONG_CLIP);
            if (active) {
                const int cursor_x = kValueX + std::min(text_width(model_.value(index), 22), 320 - kValueX - 20);
                lv_obj_t *cursor = plain_box(candidate, cursor_x + 1, y + 5, 10, 20, kColorAccent, true);
                if (!cursor) {
                    ok = false;
                    break;
                }
            }
            build.row_completed();
            y += row_h + 2;
        }

        if (ok && !status_message_.empty()) {
            lv_obj_t *status = text(candidate, lower(status_message_.c_str()), 4, kFooterY,
                                    status_error_ ? kColorError : kColorAccent, 11);
            if (status) {
                lv_obj_set_width(status, 312);
                lv_label_set_long_mode(status, LV_LABEL_LONG_CLIP);
            }
            ok = status != nullptr;
        } else if (ok) {
            ok = chips(candidate, {{"ent", "connect"}, {"tab", "save"}, {"f1", "help"}}, kFooterY);
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

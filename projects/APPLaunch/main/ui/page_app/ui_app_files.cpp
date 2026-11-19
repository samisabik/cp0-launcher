/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_files.hpp"
#include "ui_app_st.hpp"

#include "../keyboard_text_input.hpp"
#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "input_keys.h"
#include "keyboard_input.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace {

static const uint32_t &kColorAccent = cp0::theme::kAccent;
static const uint32_t &kColorText = cp0::theme::kText;
static const uint32_t &kColorIdle = cp0::theme::kIdle;
static const uint32_t &kColorDim = cp0::theme::kDim;
static const uint32_t &kColorSelect = cp0::theme::kSelect;
static const uint32_t &kColorDanger = cp0::theme::kDanger;
static const uint32_t &kColorWarn = cp0::theme::kWarn;

constexpr uint32_t kRefreshMs = 250;
constexpr uint32_t kMessageMs = 3000;
constexpr uint32_t kDeleteArmMs = 3000;
constexpr int kListTop = cp0::theme::kBodyTop + 5;
constexpr int kRowH = 18;
constexpr int kBodyBottom = cp0::theme::kFooterRuleY;
constexpr int kMetaW = 64;
constexpr std::size_t kNameChars = 33;
constexpr std::size_t kPathChars = 24;
constexpr std::size_t kNameMaxBytes = 255;
constexpr std::size_t kPreviewBytes = 4096;
constexpr const char *kEditor = "/usr/bin/micro";

std::string home_dir()
{
    const char *home = std::getenv("HOME");
    return home && home[0] ? home : "/";
}

void set_bg_opa(lv_obj_t *obj, lv_opa_t opa)
{
    if (obj && lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) != opa) lv_obj_set_style_bg_opa(obj, opa, LV_PART_MAIN);
}

void set_text(lv_obj_t *label, const std::string &text)
{
    if (!label) return;
    const char *current = lv_label_get_text(label);
    if (current && text == current) return;
    lv_label_set_text(label, text.c_str());
}

void set_color(lv_obj_t *obj, uint32_t color)
{
    if (!obj) return;
    const lv_color_t value = lv_color_hex(color);
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), value))
        lv_obj_set_style_text_color(obj, value, LV_PART_MAIN);
}

void set_visible(lv_obj_t *obj, bool visible)
{
    if (!obj) return;
    if (visible)
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *plain_box(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *box = lv_obj_create(parent);
    if (!box) return nullptr;
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    return box;
}

} // namespace

UIFilesPage::UIFilesPage() : AppPage()
{
    set_page_title("Files");
    reclaim_top_bar();
    home_ = home_dir();
    dir_ = home_;
    create_ui();
    load();
    render();
    timer_ = lv_timer_create(timer_cb, kRefreshMs, this);
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

UIFilesPage::~UIFilesPage()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
    lv_async_call_cancel(close_editor_cb, this);
    editor_.reset();
    restore_input_context();
}

void UIFilesPage::create_ui()
{
    if (!ui_APP_Container) return;
    cp0::ui::paint_background(ui_APP_Container);
    const cp0::ui::TitleRow title = cp0::ui::title_row(ui_APP_Container, "files");
    path_label_ = title.title;
    count_label_ = title.status;

    const int panel_h = kBodyBottom - cp0::theme::kBodyTop - 3;
    list_panel_ = cp0::ui::panel(ui_APP_Container, 4, cp0::theme::kBodyTop + 2, 312, panel_h).box;
    for (int i = 0; i < kRows; ++i) {
        lv_obj_t *row = plain_box(ui_APP_Container, 5, kListTop + i * kRowH, 310, kRowH);
        if (!row) continue;
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorSelect), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(kColorAccent), 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
        rows_[i] = row;
        row_name_[i] = cp0::ui::label(row, 6, 3, kColorIdle);
        if (row_name_[i]) {
            lv_obj_set_width(row_name_[i], 300 - kMetaW - 8);
            lv_label_set_long_mode(row_name_[i], LV_LABEL_LONG_CLIP);
        }
        row_meta_[i] = cp0::ui::label(row, 300 - kMetaW, 3, kColorDim);
        if (row_meta_[i]) {
            lv_obj_set_width(row_meta_[i], kMetaW);
            lv_obj_set_style_text_align(row_meta_[i], LV_TEXT_ALIGN_RIGHT, 0);
        }
    }

    preview_panel_ = cp0::ui::panel(ui_APP_Container, 4, cp0::theme::kBodyTop + 2, 312, panel_h).box;
    if (preview_panel_) {
        preview_head_ = cp0::ui::label(preview_panel_, 8, 4, kColorIdle);
        if (preview_head_) {
            lv_obj_set_width(preview_head_, 296);
            lv_label_set_long_mode(preview_head_, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_line_space(preview_head_, 2, 0);
        }
        cp0::ui::dotted_rule(preview_panel_, 8, 46, 296);
        preview_body_ = lv_obj_create(preview_panel_);
        if (preview_body_) {
            lv_obj_remove_style_all(preview_body_);
            lv_obj_set_pos(preview_body_, 8, 50);
            lv_obj_set_size(preview_body_, 298, panel_h - 54);
            lv_obj_set_scrollbar_mode(preview_body_, LV_SCROLLBAR_MODE_OFF);
            preview_text_ = cp0::ui::label(preview_body_, 0, 0, kColorText);
            if (preview_text_) {
                lv_obj_set_width(preview_text_, 296);
                lv_label_set_long_mode(preview_text_, LV_LABEL_LONG_WRAP);
                lv_obj_set_style_text_line_space(preview_text_, 2, 0);
            }
        }
    }

    const cp0::ui::Panel name = cp0::ui::panel(ui_APP_Container, 4, 46, 312, 70);
    name_panel_ = name.box;
    if (name_panel_) {
        cp0::ui::set_panel_focus(name, true);
        name_caption_ = cp0::ui::label(name_panel_, 8, 6, kColorAccent);
        name_value_ = cp0::ui::label(name_panel_, 8, 28, kColorText);
        if (name_value_) lv_label_set_long_mode(name_value_, LV_LABEL_LONG_CLIP);
        name_cursor_ = cp0::ui::box(name_panel_, 8, 28, 7, 13, kColorAccent);
        if (lv_obj_t *rule = cp0::ui::label(name_panel_, 8, 50, kColorIdle)) lv_label_set_text(rule, "no / in names");
    }

    footer_ = cp0::ui::footer(ui_APP_Container);
}

void UIFilesPage::load(const std::string &select_name)
{
    entries_ = files::list_dir(dir_, show_hidden_, list_error_);
    if (!select_name.empty()) {
        const auto found = std::find_if(entries_.begin(), entries_.end(),
                                        [&](const files::Entry &e) { return e.name == select_name; });
        selection_ = found == entries_.end() ? 0 : static_cast<int>(found - entries_.begin());
    }
    selection_ = std::clamp(selection_, 0, std::max(0, static_cast<int>(entries_.size()) - 1));
}

const files::Entry *UIFilesPage::selected() const
{
    if (selection_ < 0 || selection_ >= static_cast<int>(entries_.size())) return nullptr;
    return &entries_[static_cast<std::size_t>(selection_)];
}

std::string UIFilesPage::selected_path() const
{
    const files::Entry *entry = selected();
    return entry ? files::join(dir_, entry->name) : std::string();
}

void UIFilesPage::show_message(const std::string &text, bool error)
{
    message_ = text;
    message_color_ = error ? kColorDanger : kColorText;
    message_until_ = lv_tick_get() + kMessageMs;
}

void UIFilesPage::set_chips(const std::vector<cp0::ui::Chip> &chips)
{
    if (message_until_ != 0 && lv_tick_get() < message_until_)
        cp0::ui::set_footer_message(footer_, message_.c_str(), message_color_);
    else
        cp0::ui::set_footer(footer_, chips);
}

void UIFilesPage::render()
{
    const bool preview = view_ == View::Preview;
    set_visible(list_panel_, !preview);
    for (lv_obj_t *row : rows_) set_visible(row, !preview);
    set_visible(preview_panel_, preview);
    set_visible(name_panel_, view_ == View::Name);
    set_text(path_label_, files::display_path(dir_, home_, kPathChars));
    if (preview)
        render_preview();
    else
        render_list();
    if (view_ == View::Name) render_name();
}

void UIFilesPage::render_list()
{
    const int count = static_cast<int>(entries_.size());
    int first = std::max(0, selection_ - kRows / 2);
    if (first + kRows > count) first = std::max(0, count - kRows);
    for (int i = 0; i < kRows; ++i) {
        const int index = first + i;
        const bool has = index < count;
        const bool chosen = has && index == selection_;
        set_bg_opa(rows_[i], chosen ? LV_OPA_COVER : LV_OPA_TRANSP);
        if (rows_[i] && lv_obj_get_style_border_width(rows_[i], LV_PART_MAIN) != (chosen ? 2 : 0))
            lv_obj_set_style_border_width(rows_[i], chosen ? 2 : 0, LV_PART_MAIN);
        if (!has) {
            const bool note = i == 0 && count == 0;
            set_text(row_name_[i], note ? (list_error_.empty() ? "empty folder" : list_error_) : "");
            set_color(row_name_[i], list_error_.empty() ? kColorDim : kColorDanger);
            set_text(row_meta_[i], "");
            continue;
        }
        const files::Entry &e = entries_[static_cast<std::size_t>(index)];
        const bool marked = !clipboard_.empty() && files::join(dir_, e.name) == clipboard_;
        set_text(row_name_[i], files::clip_name(e.dir ? e.name + "/" : e.name, kNameChars));
        set_color(row_name_[i], marked ? kColorWarn : chosen ? kColorText : e.dir ? kColorAccent : kColorIdle);
        set_text(row_meta_[i], e.dir ? "dir" : files::format_size(e.size));
        set_color(row_meta_[i], chosen ? kColorIdle : kColorDim);
    }

    std::string status = std::to_string(count) + (count == 1 ? " item" : " items");
    if (show_hidden_) status = ". " + status;
    set_text(count_label_, status);

    if (view_ == View::Name) return;
    if (!clipboard_.empty())
        set_chips({{"p", clipboard_move_ ? "move here" : "copy here"}, {"ent", "open"}, {"bs", "up"}, {"esc", "home"}});
    else if (count == 0)
        set_chips({{"bs", "up"}, {"n", "new dir"}, {".", "hidden"}, {"esc", "home"}});
    else
        set_chips({{"ent", "open"}, {"e", "edit"}, {"bs", "up"}, {"r", "ren"}, {"n", "dir"}, {"dd", "del"}});
}

void UIFilesPage::render_preview()
{
    set_text(count_label_, "");
    set_chips({{"up/dn", "scroll"}, {"e", "edit"}, {"dd", "del"}, {"y/m", "cp/mv"}, {"esc", "back"}});
}

void UIFilesPage::render_name()
{
    set_text(name_caption_, name_action_ == NameAction::Rename ? "rename" : "new folder");
    // Show the end of a long name, where the cursor is.
    const std::size_t shown = 41;
    const std::string tail =
        name_text_.size() > shown ? "~" + name_text_.substr(name_text_.size() - (shown - 1)) : name_text_;
    set_text(name_value_, tail);
    if (name_cursor_ && name_value_) {
        lv_obj_update_layout(name_value_);
        lv_obj_set_x(name_cursor_, 9 + (tail.empty() ? 0 : lv_obj_get_width(name_value_)));
    }
    set_chips({{"ent", "save"}, {"bs", "del"}, {"esc", "cancel"}});
}

void UIFilesPage::open_selected()
{
    const files::Entry *entry = selected();
    if (!entry) return;
    if (!entry->dir) {
        open_preview();
        return;
    }
    dir_ = files::join(dir_, entry->name);
    selection_ = 0;
    load();
}

void UIFilesPage::go_up()
{
    if (dir_ == "/") return;
    const std::string child = files::base_name(dir_);
    dir_ = files::parent(dir_);
    load(child);
}

void UIFilesPage::open_preview()
{
    const files::Entry *entry = selected();
    if (!entry) return;
    const std::string path = files::join(dir_, entry->name);
    set_text(preview_head_, files::clip_name(entry->name, 41) + "\n" + files::format_size(entry->size) + "  " +
                                files::format_time(entry->mtime));
    set_color(preview_head_, kColorText);
    std::string text;
    if (entry->size == 0) {
        text = "(empty file)";
    } else if (!files::read_preview(path, kPreviewBytes, text)) {
        text = "(binary file, no preview)";
    } else if (entry->size > kPreviewBytes) {
        text += "\n...";
    }
    set_text(preview_text_, text);
    if (preview_body_) lv_obj_scroll_to_y(preview_body_, 0, LV_ANIM_OFF);
    view_ = View::Preview;
}

void UIFilesPage::delete_selected()
{
    const std::string path = selected_path();
    if (path.empty()) return;
    const std::string name = selected()->name;
    const std::string error = files::remove_entry(path);
    if (!error.empty()) {
        show_message("delete failed: " + error, true);
        return;
    }
    if (clipboard_ == path) clipboard_.clear();
    view_ = View::List;
    load();
    show_message("deleted " + files::clip_name(name, 30));
}

void UIFilesPage::mark_clipboard(bool move)
{
    const std::string path = selected_path();
    if (path.empty()) return;
    clipboard_ = path;
    clipboard_move_ = move;
    view_ = View::List;
    show_message(std::string(move ? "move " : "copy ") + files::clip_name(selected()->name, 24) + ": p to paste");
}

void UIFilesPage::paste_here()
{
    if (clipboard_.empty()) return;
    show_message(clipboard_move_ ? "moving..." : "copying...");
    std::string placed;
    const std::string error = files::paste(clipboard_, dir_, clipboard_move_, placed);
    if (!error.empty()) {
        show_message((clipboard_move_ ? "move failed: " : "copy failed: ") + error, true);
        return;
    }
    show_message((clipboard_move_ ? "moved " : "copied ") + files::clip_name(placed, 30));
    clipboard_.clear();
    load(placed);
}

void UIFilesPage::edit_selected()
{
    const files::Entry *entry = selected();
    if (!entry || entry->dir) return;
    if (access(kEditor, X_OK) != 0) {
        show_message("micro is not installed", true);
        return;
    }
    auto editor = std::make_shared<UISTPage>();
    if (!editor->screen()) {
        show_message("could not open a terminal", true);
        return;
    }
    editor_ = editor;
    editor_file_ = entry->name;
    editor_->terminal_sysplause = false;
    // The terminal calls this from its own callbacks, so it is torn down later.
    editor_->navigate_home = [this] { lv_async_call(close_editor_cb, this); };
    cp0_lvgl_start_app_page(*editor_);
    editor_->exec(kEditor, {files::join(dir_, entry->name)});
}

void UIFilesPage::close_editor_cb(void *user_data) noexcept
{
    auto *page = static_cast<UIFilesPage *>(user_data);
    if (!page) return;
    try {
        page->close_editor();
    } catch (...) {
    }
}

void UIFilesPage::close_editor()
{
    cp0_lvgl_start_app_page(*this);
    editor_.reset();
    view_ = View::List;
    load(editor_file_);
    render();
}

void UIFilesPage::start_name(NameAction action)
{
    if (action == NameAction::Rename && !selected()) return;
    name_action_ = action;
    name_original_ = action == NameAction::Rename ? selected()->name : std::string();
    name_text_ = name_original_;
    if (!input_context_saved_) {
        previous_input_context_ = cp0_keyboard_get_input_context();
        previous_keypad_intercept_ = cp0_keyboard_get_lvgl_keypad_intercept();
        input_context_saved_ = true;
    }
    cp0_keyboard_set_input_context(KBD_INPUT_CONTEXT_TEXT);
    cp0_keyboard_set_lvgl_keypad_intercept(1);
    message_until_ = 0;
    view_ = View::Name;
}

void UIFilesPage::restore_input_context()
{
    if (!input_context_saved_) return;
    cp0_keyboard_set_input_context(static_cast<cp0_keyboard_input_context_t>(previous_input_context_));
    cp0_keyboard_set_lvgl_keypad_intercept(previous_keypad_intercept_);
    input_context_saved_ = false;
}

void UIFilesPage::finish_name(bool save)
{
    restore_input_context();
    view_ = View::List;
    if (!save) return;
    if (name_action_ == NameAction::Rename) {
        if (name_text_ == name_original_) return;
        const std::string error = files::rename_entry(dir_, name_original_, name_text_);
        if (!error.empty()) {
            show_message("rename failed: " + error, true);
            return;
        }
        if (clipboard_ == files::join(dir_, name_original_)) clipboard_ = files::join(dir_, name_text_);
        load(name_text_);
        show_message("renamed");
    } else {
        const std::string error = files::make_dir(dir_, name_text_);
        if (!error.empty()) {
            show_message("new folder failed: " + error, true);
            return;
        }
        load(name_text_);
        show_message("created " + files::clip_name(name_text_, 30));
    }
}

void UIFilesPage::timer_cb(lv_timer_t *timer) noexcept
{
    auto *page = static_cast<UIFilesPage *>(lv_timer_get_user_data(timer));
    if (!page) return;
    try {
        if (page->message_until_ != 0 && lv_tick_get() >= page->message_until_) {
            page->message_until_ = 0;
            page->render();
        }
    } catch (...) {
    }
}

void UIFilesPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UIFilesPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item) return;
    const uint32_t key = item->semantic_key ? item->semantic_key : item->key_code;
    const bool repeat = item->key_state == KBD_KEY_REPEATED;
    try {
        if (page->view_ == View::Name) {
            if (!repeat || key == KEY_BACKSPACE) page->handle_name_key(key, item);
            return;
        }
        page->handle_key(key, repeat);
    } catch (...) {
    }
}

void UIFilesPage::handle_name_key(uint32_t key, const key_item *item)
{
    switch (key) {
    case KEY_ENTER:
    case KEY_KPENTER:
        finish_name(true);
        break;
    case KEY_ESC:
        finish_name(false);
        break;
    case KEY_BACKSPACE:
        launcher_ui::text_input::erase_last_codepoint(name_text_);
        break;
    default: {
        const std::string typed = launcher_ui::text_input::key_text(key, item);
        if (typed.find('/') == std::string::npos)
            launcher_ui::text_input::append_limited(name_text_, typed, kNameMaxBytes);
        break;
    }
    }
    render();
}

void UIFilesPage::handle_key(uint32_t key, bool repeat)
{
    const bool nav = key == KEY_UP || key == KEY_DOWN || key == KEY_BACKSPACE || key == KEY_LEFT;
    if (repeat && !nav) return;
    const bool enter = key == KEY_ENTER || key == KEY_KPENTER;
    if (key != KEY_D) delete_armed_until_ = 0;

    if (key == KEY_D) {
        if (!selected()) return;
        if (lv_tick_get() < delete_armed_until_) {
            delete_armed_until_ = 0;
            delete_selected();
        } else {
            delete_armed_until_ = lv_tick_get() + kDeleteArmMs;
            show_message(selected()->dir ? "press d again to delete folder and contents"
                                         : "press d again to delete",
                         true);
        }
        render();
        return;
    }
    if (key == KEY_E) {
        edit_selected();
        if (!editor_) render();
        return;
    }
    if (key == KEY_Y || key == KEY_M) {
        mark_clipboard(key == KEY_M);
        render();
        return;
    }

    if (view_ == View::Preview) {
        if (key == KEY_ESC || key == KEY_LEFT || key == KEY_BACKSPACE) {
            view_ = View::List;
        } else if (preview_body_ && (key == KEY_UP || key == KEY_DOWN)) {
            lv_obj_scroll_by_bounded(preview_body_, 0, key == KEY_UP ? 3 * kRowH : -3 * kRowH, LV_ANIM_OFF);
        }
        render();
        return;
    }

    const int count = static_cast<int>(entries_.size());
    switch (key) {
    case KEY_ESC:
        if (!clipboard_.empty()) {
            clipboard_.clear();
            show_message("cleared");
            break;
        }
        if (navigate_home) navigate_home();
        return;
    case KEY_UP:
        if (count > 0) selection_ = selection_ > 0 ? selection_ - 1 : (repeat ? 0 : count - 1);
        break;
    case KEY_DOWN:
        if (count > 0) selection_ = selection_ + 1 < count ? selection_ + 1 : (repeat ? count - 1 : 0);
        break;
    case KEY_RIGHT:
        open_selected();
        break;
    case KEY_LEFT:
    case KEY_BACKSPACE:
        go_up();
        break;
    case KEY_R:
        start_name(NameAction::Rename);
        break;
    case KEY_N:
        start_name(NameAction::NewFolder);
        break;
    case KEY_P:
        paste_here();
        break;
    case KEY_DOT:
        show_hidden_ = !show_hidden_;
        load(selected() ? selected()->name : std::string());
        show_message(show_hidden_ ? "showing hidden files" : "hiding hidden files");
        break;
    default:
        if (enter) open_selected();
        break;
    }
    render();
}

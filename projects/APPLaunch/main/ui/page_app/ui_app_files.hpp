/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "../model/files_model.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct key_item;
class UISTPage;

// ============================================================
//  Files  UIFilesPage
//  File browser starting in $HOME. F/X/Z/C act as arrows on this keyboard,
//  so no action uses those letters.
//
//  List:    UP/DOWN move   ENTER/RIGHT open   BACKSPACE/LEFT up a folder
//           E edit in micro   R rename   N new folder   D twice delete
//           Y copy   M move   P paste here   . hidden files   ESC home
//  Preview: UP/DOWN scroll   E edit   ESC/LEFT/BACKSPACE back   D twice delete
//  Name:    type, ENTER save, ESC cancel
// ============================================================
class UIFilesPage : public AppPage
{
public:
    UIFilesPage();
    ~UIFilesPage() override;

private:
    enum class View { List, Preview, Name };
    enum class NameAction { Rename, NewFolder };
    static constexpr int kRows = 7;

    View view_ = View::List;
    std::string dir_;
    std::string home_;
    std::vector<files::Entry> entries_;
    std::string list_error_;
    int selection_ = 0;
    bool show_hidden_ = false;

    std::string clipboard_;
    bool clipboard_move_ = false;

    NameAction name_action_ = NameAction::Rename;
    std::string name_text_;
    std::string name_original_;
    bool input_context_saved_ = false;
    int previous_input_context_ = 0;
    int previous_keypad_intercept_ = 0;

    uint32_t delete_armed_until_ = 0;
    std::string message_;
    uint32_t message_until_ = 0;
    uint32_t message_color_ = 0;

    lv_obj_t *path_label_ = nullptr;
    lv_obj_t *count_label_ = nullptr;
    lv_obj_t *list_panel_ = nullptr;
    std::array<lv_obj_t *, kRows> rows_{};
    std::array<lv_obj_t *, kRows> row_name_{};
    std::array<lv_obj_t *, kRows> row_meta_{};
    lv_obj_t *preview_panel_ = nullptr;
    lv_obj_t *preview_head_ = nullptr;
    lv_obj_t *preview_body_ = nullptr;
    lv_obj_t *preview_text_ = nullptr;
    lv_obj_t *name_panel_ = nullptr;
    lv_obj_t *name_caption_ = nullptr;
    lv_obj_t *name_value_ = nullptr;
    lv_obj_t *name_cursor_ = nullptr;
    lv_obj_t *footer_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    // micro runs in a terminal page of its own; Files comes back when it exits.
    std::shared_ptr<UISTPage> editor_;
    std::string editor_file_;

    void create_ui();
    void load(const std::string &select_name = {});
    const files::Entry *selected() const;
    std::string selected_path() const;

    void render();
    void render_list();
    void render_preview();
    void render_name();
    void show_message(const std::string &text, bool error = false);
    void set_chips(const std::vector<std::pair<const char *, const char *>> &chips);

    void open_selected();
    void go_up();
    void open_preview();
    void delete_selected();
    void mark_clipboard(bool move);
    void paste_here();
    void edit_selected();
    void close_editor();
    static void close_editor_cb(void *user_data) noexcept;
    void start_name(NameAction action);
    void finish_name(bool save);
    void restore_input_context();

    void handle_key(uint32_t key, bool repeat);
    void handle_name_key(uint32_t key, const key_item *item);
    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
};

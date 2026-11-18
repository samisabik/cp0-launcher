/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "cp0_theme.hpp"
#include "home_icon_buffer_pool.hpp"
#include "launcher_ui_app_page.hpp"
#include <array>
#include <memory>
#include <string>
#include <vector>

class Launch;

// Home screen: a rail of app types on the left and a 3x2 grid of panel tiles
// (icon and name) for the apps of the highlighted type, over a footer of key
// hints and page dots. Focus is either in the grid or in the rail: left from
// the first column enters the rail, where up and down switch type and right
// returns to the grid. Nothing animates.
class UILaunchPage : public home_base
{
public:
    static constexpr size_t kColumns = 3;
    static constexpr size_t kRows = 2;
    static constexpr size_t kTilesPerPage = kColumns * kRows;

    explicit UILaunchPage(Launch *launch);
    ~UILaunchPage();

    void show_home_screen();
    void load_home_screen();
    void start_startup_gif();
    void create_screen();

    void init_input_group();
    static void bind_home_input_group();
    static lv_group_t *home_input_group();

    void refresh_home();
    void reload_home_icons(const std::vector<std::string> &app_names);
    void launch_selected_app();

private:
    struct Tile
    {
        cp0::ui::Panel panel;
        lv_obj_t *icon = nullptr;
        lv_obj_t *name = nullptr;
    };
    // One rail row: a type and the launcher indices of its apps.
    struct Group
    {
        int category = 0;
        std::vector<size_t> apps;
    };
    static constexpr size_t kMaxPageDots = 6;
    static constexpr size_t kMaxGroups = 7;

    void create_grid(lv_obj_t *parent);
    void move_selection(int dx, int dy);
    void move_group(int delta);
    void activate();
    void set_rail_focus(bool focus);
    void rebuild_groups();
    void apply_selection();
    void apply_rail();
    void fill_page();
    void handle_home_key(lv_event_t *event);
    void forget_widgets();

    static void on_home_key(lv_event_t *event) noexcept;
    static void on_root_deleted(lv_event_t *event) noexcept;
    static void on_container_deleted(lv_event_t *event) noexcept;

    Launch *launch_ = nullptr;
    lv_obj_t *container_ = nullptr;
    std::array<Tile, kTilesPerPage> tiles_ = {};
    std::array<lv_obj_t *, kMaxPageDots> page_dots_ = {};
    std::array<lv_obj_t *, kMaxGroups> rail_labels_ = {};
    lv_obj_t *rail_highlight_ = nullptr;
    lv_obj_t *rail_marker_ = nullptr;
    lv_obj_t *hints_ = nullptr;
    std::vector<Group> groups_;
    // Group and position inside it of the launcher's current app.
    size_t group_ = 0;
    size_t position_ = 0;
    bool rail_focus_ = false;
    size_t focused_slot_ = SIZE_MAX;
    size_t shown_page_ = SIZE_MAX;
    size_t shown_group_ = SIZE_MAX;
    // SIZE_MAX forces the rail to be restyled.
    size_t rail_state_ = SIZE_MAX;
    std::vector<std::string> icon_paths_;
    HomeIconBufferPool home_icon_pool_;
    bool home_key_registered_ = false;
};

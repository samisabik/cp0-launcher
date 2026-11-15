/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "ui_launch_page.h"

#include "launch.h"
#include "launcher_platform.hpp"
#include "sample_log.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <string>

namespace {

constexpr uint32_t kColorBackground = 0x000000;
constexpr uint32_t kColorAccent = 0x3DDC84;
constexpr uint32_t kColorIdle = 0x8A8A8A;
constexpr uint32_t kColorDim = 0x5F7A66;
constexpr uint32_t kColorText = 0xE6E6E6;

constexpr lv_coord_t kGridLeft = 16;
constexpr lv_coord_t kGridTop = 6;
constexpr lv_coord_t kTileWidth = 66;
constexpr lv_coord_t kTileHeight = 46;
constexpr lv_coord_t kColumnStep = 74;
constexpr lv_coord_t kRowStep = 52;
constexpr lv_coord_t kFooterTop = 114;
constexpr lv_coord_t kCornerSize = 8;
constexpr lv_coord_t kCornerWidth = 2;
constexpr uint32_t kIconSize = 24;

UILaunchPage *active_launch_page = nullptr;
lv_group_t *home_input_group = nullptr;

std::string app_slug(const std::string &name)
{
    std::string slug;
    slug.reserve(name.size());
    for (unsigned char c : name)
        slug.push_back(c == ' ' ? '_' : static_cast<char>(std::tolower(c)));
    return slug;
}

std::string line_icon_path(const std::string &name)
{
    static const std::set<std::string> kKnown = {
        "cli", "factorytest", "files", "gps", "ip_panel",
        "ir_chat", "ir_remote", "keyboard_guide", "lora", "micro", "nfc", "power", "python",
        "rec", "sdr", "settings", "spectrum", "ssh", "subg_chat",
    };
    const std::string slug = app_slug(name);
    return launcher_platform::path("line/" + (kKnown.count(slug) ? slug : std::string("app")) + ".png");
}

lv_obj_t *plain_obj(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (!obj) return nullptr;
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return obj;
}

lv_obj_t *mono_label(lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    if (!label) return nullptr;
    lv_obj_set_style_text_font(label, launcher_fonts().get_chrome(kCp0ChromeFontSize),
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    return label;
}

} // namespace

lv_group_t *UILaunchPage::home_input_group()
{
    if (active_launch_page)
        return active_launch_page->input_group();
    return ::home_input_group;
}

void UILaunchPage::bind_home_input_group()
{
    if (lv_indev_t *indev = lv_indev_get_next(nullptr))
        lv_indev_set_group(indev, home_input_group());
}

void UILaunchPage::init_input_group()
{
    ::home_input_group = input_group();
    bind_home_input_group();
}

UILaunchPage::UILaunchPage(Launch *launch)
    : home_base(), launch_(launch)
{
    active_launch_page = this;
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, UILaunchPage::on_root_deleted, LV_EVENT_DELETE, this);
}

UILaunchPage::~UILaunchPage()
{
    if (container_) {
        lv_obj_remove_event_cb_with_user_data(container_, UILaunchPage::on_container_deleted, this);
        lv_obj_delete(container_);
    }
    forget_widgets();
    if (home_key_registered_ && root_screen_)
        lv_obj_remove_event_cb_with_user_data(root_screen_, UILaunchPage::on_home_key, this);
    if (root_screen_)
        lv_obj_remove_event_cb_with_user_data(root_screen_, UILaunchPage::on_root_deleted, this);
    if (active_launch_page == this)
        active_launch_page = nullptr;
    if (::home_input_group == input_group())
        ::home_input_group = nullptr;
}

void UILaunchPage::forget_widgets()
{
    container_ = nullptr;
    tiles_.fill({});
    corners_.fill(nullptr);
    selector_ = nullptr;
    command_label_ = nullptr;
    page_label_ = nullptr;
    shown_page_ = SIZE_MAX;
}

void UILaunchPage::on_root_deleted(lv_event_t *event) noexcept
{
    auto *self = static_cast<UILaunchPage *>(lv_event_get_user_data(event));
    if (!self || lv_event_get_target(event) != lv_event_get_current_target(event)) return;
    self->forget_widgets();
    self->home_key_registered_ = false;
}

void UILaunchPage::on_container_deleted(lv_event_t *event) noexcept
{
    auto *self = static_cast<UILaunchPage *>(lv_event_get_user_data(event));
    if (!self || lv_event_get_target(event) != lv_event_get_current_target(event)) return;
    self->forget_widgets();
}

void UILaunchPage::create_screen()
{
    if (!container_)
        create_grid(content_container());
}

void UILaunchPage::create_grid(lv_obj_t *parent)
{
    if (!parent) return;
    lv_obj_set_style_bg_color(parent, lv_color_hex(kColorBackground), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *container = plain_obj(parent);
    if (!container) return;
    lv_obj_set_size(container, 320, 150);
    lv_obj_set_align(container, LV_ALIGN_TOP_LEFT);

    for (size_t i = 0; i < kTilesPerPage; ++i) {
        Tile &tile = tiles_[i];
        tile.box = plain_obj(container);
        if (!tile.box) continue;
        lv_obj_set_size(tile.box, kTileWidth, kTileHeight);
        lv_obj_set_pos(tile.box, kGridLeft + static_cast<lv_coord_t>(i % kColumns) * kColumnStep,
                       kGridTop + static_cast<lv_coord_t>(i / kColumns) * kRowStep);
        tile.icon = lv_image_create(tile.box);
        if (!tile.icon) continue;
        lv_obj_center(tile.icon);
        lv_obj_set_style_image_recolor_opa(tile.icon, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_image_recolor(tile.icon, lv_color_hex(kColorIdle), LV_PART_MAIN);
    }

    selector_ = plain_obj(container);
    if (selector_) {
        lv_obj_set_size(selector_, kTileWidth, kTileHeight);
        static constexpr lv_align_t kAligns[4] = {
            LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_RIGHT, LV_ALIGN_BOTTOM_LEFT, LV_ALIGN_BOTTOM_RIGHT};
        static constexpr lv_border_side_t kSides[4] = {
            static_cast<lv_border_side_t>(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_TOP),
            static_cast<lv_border_side_t>(LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_TOP),
            static_cast<lv_border_side_t>(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_BOTTOM),
            static_cast<lv_border_side_t>(LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM)};
        for (size_t i = 0; i < corners_.size(); ++i) {
            lv_obj_t *corner = plain_obj(selector_);
            if (!corner) continue;
            lv_obj_set_size(corner, kCornerSize, kCornerSize);
            lv_obj_set_align(corner, kAligns[i]);
            lv_obj_set_style_border_color(corner, lv_color_hex(kColorAccent), LV_PART_MAIN);
            lv_obj_set_style_border_opa(corner, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_width(corner, kCornerWidth, LV_PART_MAIN);
            lv_obj_set_style_border_side(corner, kSides[i], LV_PART_MAIN);
            corners_[i] = corner;
        }
    }

    lv_obj_t *footer = plain_obj(container);
    if (footer) {
        lv_obj_set_size(footer, 288, 26);
        lv_obj_set_pos(footer, kGridLeft, kFooterTop);
        lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        if (lv_obj_t *prompt = mono_label(footer, kColorDim))
            lv_label_set_text(prompt, "$ ");
        command_label_ = mono_label(footer, kColorText);
        if (lv_obj_t *spacer = plain_obj(footer)) {
            lv_obj_set_height(spacer, 1);
            lv_obj_set_flex_grow(spacer, 1);
        }
        page_label_ = mono_label(footer, kColorDim);
    }

    container_ = container;
    lv_obj_add_event_cb(container_, on_container_deleted, LV_EVENT_DELETE, this);
    if (!home_key_registered_ && screen()) {
        lv_obj_add_event_cb(screen(), on_home_key, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
        home_key_registered_ = true;
    }
}

void UILaunchPage::reload_home_icons(const std::vector<std::string> &app_names)
{
    icon_paths_.clear();
    icon_paths_.reserve(app_names.size());
    for (const std::string &name : app_names)
        icon_paths_.push_back(line_icon_path(name));

    HomeIconBufferPool replacement;
    replacement.rebuild(icon_paths_, kIconSize);
    // Detach the images before the old buffers are released.
    for (Tile &tile : tiles_)
        if (tile.icon) lv_image_set_src(tile.icon, nullptr);
    home_icon_pool_.swap(replacement);
    shown_page_ = SIZE_MAX;
    refresh_home();
}

void UILaunchPage::fill_page()
{
    const size_t count = launch_ ? launch_->app_count() : 0;
    const size_t selected = launch_ ? launch_->current_app_index() : 0;
    const size_t page = selected / kTilesPerPage;
    if (page == shown_page_) return;
    shown_page_ = page;

    for (size_t i = 0; i < kTilesPerPage; ++i) {
        Tile &tile = tiles_[i];
        if (!tile.box || !tile.icon) continue;
        const size_t index = page * kTilesPerPage + i;
        if (index >= count || index >= icon_paths_.size()) {
            lv_obj_add_flag(tile.box, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(tile.box, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(tile.icon, home_icon_pool_.find(icon_paths_[index]));
    }

    if (page_label_) {
        const size_t pages = count == 0 ? 1 : (count + kTilesPerPage - 1) / kTilesPerPage;
        lv_label_set_text_fmt(page_label_, "%u/%u", static_cast<unsigned>(page + 1),
                              static_cast<unsigned>(pages));
    }
}

void UILaunchPage::apply_selection()
{
    if (!launch_ || !container_) return;
    const size_t selected = launch_->current_app_index();
    const size_t slot = selected % kTilesPerPage;

    for (size_t i = 0; i < kTilesPerPage; ++i) {
        if (!tiles_[i].icon) continue;
        // Only the previous and new selection change colour; skip the rest.
        const lv_color_t color = lv_color_hex(i == slot ? kColorAccent : kColorIdle);
        if (lv_color_eq(lv_obj_get_style_image_recolor(tiles_[i].icon, LV_PART_MAIN), color)) continue;
        lv_obj_set_style_image_recolor(tiles_[i].icon, color, LV_PART_MAIN);
    }
    if (selector_) {
        lv_obj_set_pos(selector_, kGridLeft + static_cast<lv_coord_t>(slot % kColumns) * kColumnStep,
                       kGridTop + static_cast<lv_coord_t>(slot / kColumns) * kRowStep);
        if (launch_->app_count() == 0) lv_obj_add_flag(selector_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(selector_, LV_OBJ_FLAG_HIDDEN);
    }
    if (command_label_) {
        const app *item = launch_->app_at(selected);
        const std::string command = item ? "./" + app_slug(item->Name) : std::string();
        if (std::strcmp(lv_label_get_text(command_label_), command.c_str()) != 0)
            lv_label_set_text(command_label_, command.c_str());
    }
}

void UILaunchPage::refresh_home()
{
    fill_page();
    apply_selection();
}

void UILaunchPage::move_selection(int delta)
{
    if (!launch_) return;
    const int count = static_cast<int>(launch_->app_count());
    if (count <= 0) return;
    int next = static_cast<int>(launch_->current_app_index()) + delta;
    next = ((next % count) + count) % count;
    launch_->set_current_app_index(static_cast<size_t>(next));
    refresh_home();
}

void UILaunchPage::launch_selected_app()
{
    if (launch_)
        launch_->launch_app();
}

void UILaunchPage::show_home_screen()
{
    if (!screen()) return;
    refresh_home();
    lv_disp_load_scr(screen());
    bind_home_input_group();
}

void UILaunchPage::load_home_screen()
{
    show_home_screen();
}

void UILaunchPage::start_startup_gif()
{
    show_home_screen();
}

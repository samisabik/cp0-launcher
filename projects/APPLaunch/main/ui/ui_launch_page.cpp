/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "ui_launch_page.h"

#include "cp0_lvgl_app.h"
#include "launch.h"
#include "launcher_platform.hpp"
#include "sample_log.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <string>

namespace {

namespace theme = cp0::theme;

// Content area is 320x150 under the 20 px status bar.
constexpr lv_coord_t kContentH = 150;
constexpr lv_coord_t kGridLeft = 4;
constexpr lv_coord_t kGridTop = 2;
constexpr lv_coord_t kTileWidth = 101;
constexpr lv_coord_t kTileHeight = 62;
constexpr lv_coord_t kColumnStep = 105;
constexpr lv_coord_t kRowStep = 66;
constexpr lv_coord_t kFooterRuleY = 133;
constexpr lv_coord_t kFooterY = 136;
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
        "adsb", "air", "audio", "cli", "files", "fox", "ip_panel", "ir", "keys", "micro",
        "pins", "python", "rec", "sat", "sdr", "sensors", "settings", "slime", "spectrum", "ssh",
        "tailnet", "tracker",
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
    page_dots_.fill(nullptr);
    focused_slot_ = SIZE_MAX;
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
    lv_obj_set_style_bg_color(parent, lv_color_hex(theme::kBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *container = plain_obj(parent);
    if (!container) return;
    lv_obj_set_size(container, theme::kScreenW, kContentH);
    lv_obj_set_align(container, LV_ALIGN_TOP_LEFT);
    cp0::ui::dot_grid(container, 0, 0, theme::kScreenW, kFooterRuleY);

    for (size_t i = 0; i < kTilesPerPage; ++i) {
        Tile &tile = tiles_[i];
        tile.panel = cp0::ui::panel(container, kGridLeft + static_cast<lv_coord_t>(i % kColumns) * kColumnStep,
                                    kGridTop + static_cast<lv_coord_t>(i / kColumns) * kRowStep, kTileWidth,
                                    kTileHeight);
        if (!tile.panel.box) continue;
        tile.icon = lv_image_create(tile.panel.box);
        if (tile.icon) {
            lv_obj_set_pos(tile.icon, 8, 8);
            lv_obj_set_style_image_recolor_opa(tile.icon, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_image_recolor(tile.icon, lv_color_hex(theme::kIdle), LV_PART_MAIN);
        }
        tile.name = cp0::ui::label(tile.panel.box, 8, 44, theme::kIdle);
        if (tile.name) {
            lv_obj_set_width(tile.name, kTileWidth - 12);
            lv_label_set_long_mode(tile.name, LV_LABEL_LONG_CLIP);
        }
    }

    cp0::ui::dotted_rule(container, 0, kFooterRuleY, theme::kScreenW);
    if (lv_obj_t *hints = cp0::ui::label(container, theme::kPadX, kFooterY, theme::kIdle)) {
        lv_obj_set_width(hints, 220);
        lv_label_set_recolor(hints, true);
        cp0::ui::set_footer(hints, {{"ent", "open"}, {"tab", "lock"}});
    }
    for (size_t i = 0; i < page_dots_.size(); ++i) {
        page_dots_[i] = cp0::ui::box(container, 0, kFooterY + 4, 4, 4, theme::kDim);
        if (page_dots_[i]) lv_obj_add_flag(page_dots_[i], LV_OBJ_FLAG_HIDDEN);
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
    focused_slot_ = SIZE_MAX;

    for (size_t i = 0; i < kTilesPerPage; ++i) {
        Tile &tile = tiles_[i];
        if (!tile.panel.box || !tile.icon) continue;
        const size_t index = page * kTilesPerPage + i;
        const app *item = index < count ? launch_->app_at(index) : nullptr;
        if (!item || index >= icon_paths_.size()) {
            lv_obj_add_flag(tile.panel.box, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(tile.panel.box, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(tile.icon, home_icon_pool_.find(icon_paths_[index]));
        std::string name = item->Name;
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // 13 columns fit in a tile; longer names fall back to their first word.
        if (name.size() > 13 && name.find(' ') != std::string::npos) name = name.substr(0, name.find(' '));
        if (tile.name) lv_label_set_text(tile.name, name.c_str());
        cp0::ui::set_panel_focus(tile.panel, false);
        lv_obj_set_style_image_recolor(tile.icon, lv_color_hex(theme::kIdle), LV_PART_MAIN);
        if (tile.name) lv_obj_set_style_text_color(tile.name, lv_color_hex(theme::kIdle), 0);
    }

    const size_t pages = count == 0 ? 1 : (count + kTilesPerPage - 1) / kTilesPerPage;
    const size_t shown = std::min(pages, page_dots_.size());
    for (size_t i = 0; i < page_dots_.size(); ++i) {
        lv_obj_t *dot = page_dots_[i];
        if (!dot) continue;
        if (i >= shown || pages < 2) {
            lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(dot, static_cast<lv_coord_t>(theme::kScreenW - theme::kPadX - 4 - (shown - 1 - i) * 8));
        lv_obj_set_style_bg_color(dot, lv_color_hex(i == page ? theme::kAccent : theme::kDim), 0);
    }
}

void UILaunchPage::apply_selection()
{
    if (!launch_ || !container_) return;
    const size_t slot = launch_->app_count() == 0 ? SIZE_MAX : launch_->current_app_index() % kTilesPerPage;
    if (slot == focused_slot_) return;
    for (size_t i : {focused_slot_, slot}) {
        if (i >= kTilesPerPage) continue;
        Tile &tile = tiles_[i];
        const bool on = i == slot;
        cp0::ui::set_panel_focus(tile.panel, on);
        if (tile.icon)
            lv_obj_set_style_image_recolor(tile.icon, lv_color_hex(on ? theme::kAccent : theme::kIdle), LV_PART_MAIN);
        if (tile.name) lv_obj_set_style_text_color(tile.name, lv_color_hex(on ? theme::kText : theme::kIdle), 0);
    }
    focused_slot_ = slot;
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
    shown_page_ = SIZE_MAX;
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

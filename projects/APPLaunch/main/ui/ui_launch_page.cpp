/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "ui_launch_page.h"

#include "app_display_order.hpp"
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
constexpr lv_coord_t kRailWidth = 66;
constexpr lv_coord_t kRailTextX = 7;
constexpr lv_coord_t kRailRowH = 19;
constexpr lv_coord_t kGridLeft = 71;
constexpr lv_coord_t kGridTop = 2;
constexpr lv_coord_t kTileWidth = 80;
constexpr lv_coord_t kTileHeight = 62;
constexpr lv_coord_t kColumnStep = 83;
constexpr lv_coord_t kRowStep = 66;
// Columns of text that fit in a tile.
constexpr std::size_t kNameColumns = 9;
constexpr lv_coord_t kFooterRuleY = 133;
constexpr lv_coord_t kFooterY = 136;
constexpr uint32_t kIconSize = 24;
constexpr const char *kFallbackIcon = "app";

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
        "adsb", "air", "audio", "cli", "files", "fox", "gps", "ip_panel", "ir", "keys",
        "pd", "pins", "portal", "power", "python", "rec", "sat", "sdr", "sensors", "settings", "slime",
        "spectrum", "ssh", "tailnet", "tracker",
    };
    const std::string slug = app_slug(name);
    return launcher_platform::path("line/" + (kKnown.count(slug) ? slug : std::string(kFallbackIcon)) + ".png");
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
    rail_labels_.fill(nullptr);
    rail_highlight_ = nullptr;
    rail_marker_ = nullptr;
    hints_ = nullptr;
    focused_slot_ = SIZE_MAX;
    shown_page_ = SIZE_MAX;
    shown_group_ = SIZE_MAX;
    rail_state_ = SIZE_MAX;
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

    rail_highlight_ = cp0::ui::box(container, 0, 0, kRailWidth, kRailRowH, theme::kPanel);
    rail_marker_ = cp0::ui::box(container, 0, 0, 2, kRailRowH, theme::kBracket);
    for (lv_obj_t *&label : rail_labels_) {
        label = cp0::ui::label(container, kRailTextX, 0, theme::kIdle);
        if (label) lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
    cp0::ui::tiled(container, kRailWidth, 0, 1, kFooterRuleY, cp0::ui::pattern(1, 2, theme::kLine));

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
    hints_ = cp0::ui::label(container, theme::kPadX, kFooterY, theme::kIdle);
    if (hints_) {
        lv_obj_set_width(hints_, 240);
        lv_label_set_recolor(hints_, true);
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
    replacement.rebuild(icon_paths_, launcher_platform::path(std::string("line/") + kFallbackIcon + ".png"), kIconSize);
    // Detach the images before the old buffers are released.
    for (Tile &tile : tiles_)
        if (tile.icon) lv_image_set_src(tile.icon, nullptr);
    home_icon_pool_.swap(replacement);
    shown_page_ = SIZE_MAX;
    refresh_home();
}

// Sorts the launcher's apps into the rail's types and finds the current app
// among them. Types without apps get no row.
void UILaunchPage::rebuild_groups()
{
    groups_.clear();
    group_ = 0;
    position_ = 0;
    const size_t count = launch_ ? launch_->app_count() : 0;
    const size_t selected = launch_ ? launch_->current_app_index() : 0;
    std::array<std::vector<size_t>, kHomeCategoryCount> by_category;
    for (size_t index = 0; index < count; ++index) {
        const app *item = launch_->app_at(index);
        if (!item) continue;
        by_category[static_cast<size_t>(launcher_home_category(item->Name))].push_back(index);
    }
    for (int category = 0; category < kHomeCategoryCount; ++category) {
        std::vector<size_t> &apps = by_category[static_cast<size_t>(category)];
        if (apps.empty() || groups_.size() >= kMaxGroups) continue;
        const auto found = std::find(apps.begin(), apps.end(), selected);
        if (found != apps.end()) {
            group_ = groups_.size();
            position_ = static_cast<size_t>(found - apps.begin());
        }
        groups_.push_back({category, std::move(apps)});
    }
}

void UILaunchPage::fill_page()
{
    const size_t page = position_ / kTilesPerPage;
    if (page == shown_page_ && group_ == shown_group_) return;
    shown_page_ = page;
    shown_group_ = group_;
    focused_slot_ = SIZE_MAX;
    rail_state_ = SIZE_MAX;
    static const std::vector<size_t> kNone;
    const std::vector<size_t> &apps = group_ < groups_.size() ? groups_[group_].apps : kNone;

    for (size_t i = 0; i < kTilesPerPage; ++i) {
        Tile &tile = tiles_[i];
        if (!tile.panel.box || !tile.icon) continue;
        const size_t slot = page * kTilesPerPage + i;
        const size_t index = slot < apps.size() ? apps[slot] : SIZE_MAX;
        const app *item = index != SIZE_MAX ? launch_->app_at(index) : nullptr;
        if (!item || index >= icon_paths_.size()) {
            lv_obj_add_flag(tile.panel.box, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(tile.panel.box, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(tile.icon, home_icon_pool_.find(icon_paths_[index]));
        std::string name = item->Name;
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // Longer names fall back to their first word.
        if (name.size() > kNameColumns && name.find(' ') != std::string::npos) name = name.substr(0, name.find(' '));
        if (tile.name) lv_label_set_text(tile.name, name.c_str());
        cp0::ui::set_panel_focus(tile.panel, false);
        lv_obj_set_style_image_recolor(tile.icon, lv_color_hex(theme::kIdle), LV_PART_MAIN);
        if (tile.name) lv_obj_set_style_text_color(tile.name, lv_color_hex(theme::kIdle), 0);
    }

    const size_t pages = (apps.size() + kTilesPerPage - 1) / kTilesPerPage;
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
    const size_t slot = groups_.empty() || rail_focus_ ? SIZE_MAX : position_ % kTilesPerPage;
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

void UILaunchPage::apply_rail()
{
    if (!container_) return;
    const size_t state = group_ * 2 + (rail_focus_ ? 1 : 0);
    if (state == rail_state_) return;
    rail_state_ = state;

    // Rows share the rail's height, so a seventh type still fits.
    const lv_coord_t step = groups_.size() > 6 ? 18 : 21;
    for (size_t i = 0; i < rail_labels_.size(); ++i) {
        lv_obj_t *label = rail_labels_[i];
        if (!label) continue;
        if (i >= groups_.size()) {
            lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(label, static_cast<lv_coord_t>(4 + static_cast<lv_coord_t>(i) * step));
        const uint32_t color = i != group_ ? theme::kIdle : rail_focus_ ? theme::kText : theme::kAccent;
        cp0::ui::set_text(label, launcher_home_category_name(groups_[i].category), color);
    }
    for (lv_obj_t *obj : {rail_highlight_, rail_marker_}) {
        if (!obj) continue;
        if (groups_.empty()) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(obj, static_cast<lv_coord_t>(static_cast<lv_coord_t>(group_) * step));
    }
    cp0::ui::set_bg_color(rail_highlight_, rail_focus_ ? theme::kSelect : theme::kPanel);
    cp0::ui::set_bg_color(rail_marker_, rail_focus_ ? theme::kAccent : theme::kBracket);
    if (rail_focus_)
        cp0::ui::set_footer(hints_, {{"up/dn", "type"}, {">", "apps"}, {"tab", "lock"}});
    else
        cp0::ui::set_footer(hints_, {{"ent", "open"}, {"<", "types"}, {"tab", "lock"}});
}

void UILaunchPage::refresh_home()
{
    rebuild_groups();
    fill_page();
    apply_selection();
    apply_rail();
}

void UILaunchPage::set_rail_focus(bool focus)
{
    if (rail_focus_ == focus) return;
    rail_focus_ = focus;
    refresh_home();
}

// Selects the first app of the type `delta` rows away, wrapping.
void UILaunchPage::move_group(int delta)
{
    const int count = static_cast<int>(groups_.size());
    if (!launch_ || count == 0) return;
    const int next = ((static_cast<int>(group_) + delta) % count + count) % count;
    launch_->set_current_app_index(groups_[static_cast<size_t>(next)].apps.front());
    refresh_home();
}

void UILaunchPage::move_selection(int dx, int dy)
{
    if (!launch_ || group_ >= groups_.size()) return;
    if (rail_focus_) {
        if (dy != 0)
            move_group(dy);
        else if (dx > 0)
            set_rail_focus(false);
        return;
    }
    const std::vector<size_t> &apps = groups_[group_].apps;
    const size_t columns = kColumns;
    size_t next = position_;
    if (dx < 0) {
        if (position_ % columns == 0) {
            set_rail_focus(true);
            return;
        }
        --next;
    } else if (dx > 0) {
        if (position_ + 1 < apps.size()) ++next;
    } else if (dy < 0) {
        if (position_ >= columns) next -= columns;
    } else if (dy > 0) {
        // A short last row is still reachable from the columns it lacks.
        if (position_ + columns < apps.size())
            next += columns;
        else if (position_ / columns < (apps.size() - 1) / columns)
            next = apps.size() - 1;
    }
    if (next == position_) return;
    launch_->set_current_app_index(apps[next]);
    refresh_home();
}

void UILaunchPage::activate()
{
    if (rail_focus_)
        set_rail_focus(false);
    else
        launch_selected_app();
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

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lvgl/lvgl.h"
#include "cp0_lvgl_app.h"
#include "cp0_lvgl_app_page_assets.h"
#include "hal_lvgl_bsp.h"

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>

class AppTopBarRegion;
class AppTopBarComponent;
namespace cp0::status { class Lifecycle; }

/**
 * @file ui_app_page.hpp
 * @brief Page, content-region, and top-bar building blocks.
 *
 * Page inheritance guide:
 * - Inherit from AppPageRoot for the smallest possible blank LVGL screen.
 * - Inherit from AppPage for a normal application page with a content region
 *   and an enabled top bar.
 *
 * AppPageRoot creates and owns the LVGL screen. The screen is available from
 * screen() after the base constructor has completed. A page is shown with
 * cp0_lvgl_start_app_page(page).
 *
 * Minimal blank-page example:
 * @code
 * class BlankPage final : public AppPageRoot
 * {
 * public:
 *     BlankPage()
 *     {
 *         lv_obj_set_style_bg_color(screen(), lv_color_hex(0x101010), 0);
 *     }
 * };
 *
 * BlankPage page;
 * cp0_lvgl_start_app_page(page);
 * @endcode
 *
 * Use AppPage instead when the page should participate in the standard
 * application layout:
 * @code
 * class StandardPage final : public AppPage
 * {
 * public:
 *     StandardPage()
 *     {
 *         set_page_title("Standard page");
 *         lv_obj_t *label = lv_label_create(ui_APP_Container);
 *         lv_label_set_text(label, "Hello");
 *         lv_obj_center(label);
 *     }
 * };
 * @endcode
 */

/**
 * @brief Owns the reusable application content container.
 *
 * This helper is normally used by AppContentRegion. Application pages should
 * usually inherit from AppPageRoot or AppPage instead of using this class
 * directly.
 */
class UIAppContainer
{
public:
    UIAppContainer() = default;
    explicit UIAppContainer(int height);
    ~UIAppContainer();

    void set_height(int height);
    lv_obj_t *create(lv_obj_t *parent);

    lv_obj_t *get() const { return container_; }

private:
    static void container_delete_cb(lv_event_t *event);
    int height_ = 150;
    lv_obj_t *container_ = nullptr;
};

/**
 * @brief Minimal page base that owns one LVGL screen and its input group.
 *
 * Derive directly from this class to create a blank or custom screen without
 * the standard content-area and top-bar layout. The constructor creates the
 * screen, and screen() returns the root LVGL object for custom UI creation.
 */
class AppPageRoot
{
public:
    /** Height in pixels of the standard top bar region. Layout consumers that
     *  must stay clear of the bar use this instead of repeating the value. */
    static constexpr int kTopBarHeightPx = 20;

    AppPageRoot();
    virtual ~AppPageRoot();

    AppPageRoot(const AppPageRoot &) = delete;
    AppPageRoot &operator=(const AppPageRoot &) = delete;
    AppPageRoot(AppPageRoot &&) = delete;
    AppPageRoot &operator=(AppPageRoot &&) = delete;

    lv_obj_t *screen() const { return root_screen_; }
    lv_group_t *input_group() const { return input_group_; }

    void enable_top_bar();
    void disable_top_bar();
    void set_page_title(const std::string &title);

    std::string page_title_ = "APP";
    lv_group_t *input_group_ = nullptr;
    lv_obj_t *root_screen_ = nullptr;
    std::function<void()> navigate_home;
    int top_bar_height_px_ = kTopBarHeightPx;

private:
    static void screen_delete_cb(lv_event_t *event);
    void create_base_ui();
    void create_input_group();
    void release_root_screen();

    std::unique_ptr<AppTopBarRegion> top_bar_region_;
};

/**
 * @brief Adds the standard application content container to AppPageRoot.
 *
 * The content container is available as ui_APP_Container. This class is used
 * as a layout building block and is normally reached through AppPage.
 */
class AppContentRegion : virtual public AppPageRoot
{
public:
    AppContentRegion();

    virtual ~AppContentRegion();

    void refresh();

    // Full-bleed mode: hide the 20 px top bar and grow the content container to
    // the whole 320x170 screen. For apps that want every pixel and do not need
    // the clock/battery chrome while running.
    void reclaim_top_bar();

    lv_obj_t *ui_APP_Container = nullptr;

private:
    static void container_delete_cb(lv_event_t *event);
    UIAppContainer app_container_;
};

/**
 * @brief Base class for a component mounted inside the application's top bar.
 *
 * Derived components implement on_create() to populate the object returned by
 * obj(). The top-bar manager owns the LVGL object and controls its visibility.
 */
class AppTopBarComponent
{
public:
    // Components live inside a 20 px bar with 2 px vertical clearance.
    static constexpr int kMaximumHeight = 16;
    // Used when a component does not provide a useful width recommendation.
    static constexpr int kDefaultRecommendedWidth = 20;

    explicit AppTopBarComponent(std::string id,
                                int recommended_width = kDefaultRecommendedWidth,
                                int height = kMaximumHeight);
    virtual ~AppTopBarComponent();

    AppTopBarComponent(const AppTopBarComponent &) = delete;
    AppTopBarComponent &operator=(const AppTopBarComponent &) = delete;
    AppTopBarComponent(AppTopBarComponent &&) = delete;
    AppTopBarComponent &operator=(AppTopBarComponent &&) = delete;

    const std::string &id() const { return id_; }
    int height() const { return height_; }
    int recommended_width() const { return recommended_width_; }
    lv_obj_t *obj() const { return obj_; }

    void set_visible(bool visible);

    virtual void deactivate() {}
    virtual void refresh() {}

protected:
    // The base class creates and owns obj. Derived components add their LVGL
    // children here and can use obj() later for state updates.
    virtual void on_create(lv_obj_t *obj) = 0;
    virtual void on_object_deleted() {}
    virtual bool resources_ready() const { return true; }
    void set_width(int width);
    void unmount();

private:
    friend class UIAppTopBar;

    static void object_delete_cb(lv_event_t *event);
    lv_obj_t *mount(lv_obj_t *parent);
    void detach_from_deleted_parent();

    std::string id_;
    int recommended_width_;
    int height_;
    lv_obj_t *obj_ = nullptr;
};

/** @brief Built-in top-bar component that displays the current page title. */
class AppTopBarTitleComponent final : public AppTopBarComponent
{
public:
    AppTopBarTitleComponent();
    ~AppTopBarTitleComponent() override;
    void set_title(const std::string &title);

protected:
    void on_create(lv_obj_t *obj) override;
    void on_object_deleted() override;
    bool resources_ready() const override;

private:
    static void child_delete_cb(lv_event_t *event);
    std::string title_ = "APP";
    lv_obj_t *label_ = nullptr;
};

/** @brief Built-in top-bar component that displays the current time. */
class AppTopBarTimeComponent final : public AppTopBarComponent
{
public:
    AppTopBarTimeComponent();
    ~AppTopBarTimeComponent() override;
    void refresh() override;

protected:
    void on_create(lv_obj_t *obj) override;
    void on_object_deleted() override;
    bool resources_ready() const override;

private:
    static void child_delete_cb(lv_event_t *event);
    lv_obj_t *label_ = nullptr;
};

/** @brief Built-in top-bar component that displays battery state. */
class AppTopBarBatteryComponent final : public AppTopBarComponent
{
public:
    AppTopBarBatteryComponent();
    ~AppTopBarBatteryComponent() override;
    void update(const cp0_battery_info_t &battery);
    void deactivate() override;

protected:
    void on_create(lv_obj_t *obj) override;
    void on_object_deleted() override;
    bool resources_ready() const override;

private:
    static void child_delete_cb(lv_event_t *event);
    static void blink_timer_cb(lv_timer_t *timer);
    void set_charging(bool charging);
    void set_low_battery_blink(bool enabled);

    bool charging_ = false;
    bool charging_styled_ = false;
    bool low_battery_blink_ = false;
    bool blink_dim_ = false;
    lv_timer_t *blink_timer_ = nullptr;
    lv_obj_t *battery_bar_ = nullptr;
    lv_obj_t *charge_wave_ = nullptr;
    lv_obj_t *label_ = nullptr;
};

/** @brief Built-in top-bar component that displays network state. */
class AppTopBarNetworkComponent final : public AppTopBarComponent
{
public:
    AppTopBarNetworkComponent();
    ~AppTopBarNetworkComponent() override;
    void refresh() override;

protected:
    void on_create(lv_obj_t *obj) override;
    void on_object_deleted() override;
    bool resources_ready() const override;

private:
    static void child_delete_cb(lv_event_t *event);
    lv_obj_t *ethernet_image_ = nullptr;
    lv_obj_t *wifi_panel_ = nullptr;
    lv_obj_t *wifi_bars_[4] = {};
    // Bit-packed signal bars, ethernet and wifi flags last drawn; -1 forces a redraw.
    int drawn_state_ = -1;
};

/**
 * @brief Owns and manages the top-bar container and its components.
 *
 * Pages normally access this functionality through AppPageRoot rather than
 * constructing UIAppTopBar directly.
 */
class UIAppTopBar
{
public:
    UIAppTopBar();
    ~UIAppTopBar();
    void set_height(int height) { height_ = height; }

    lv_obj_t *create(lv_obj_t *parent);
    void set_title(const std::string &title);
    void update_time();
    void update_network();
    void update_battery(const cp0_battery_info_t &battery);
    void set_visible(bool visible);
    void set_active(bool active);
    void detach_from_deleted_parent();
    lv_obj_t *container() const { return container_; }

private:
    friend class AppTopBarRegion;

    void set_delete_callback(std::function<void()> callback)
    {
        delete_callback_ = std::move(callback);
    }
    static void container_delete_cb(lv_event_t *event);
    int height_ = 20;
    bool active_ = false;
    lv_obj_t *container_ = nullptr;
    std::unique_ptr<AppTopBarTitleComponent> title_;
    std::unique_ptr<AppTopBarNetworkComponent> network_;
    std::unique_ptr<AppTopBarTimeComponent> time_;
    std::unique_ptr<AppTopBarBatteryComponent> battery_;
    std::function<void()> delete_callback_;
};

/**
 * @brief Connects a page root to its optional top-bar layout and components.
 *
 * AppPageRoot owns one of these regions internally. Most application pages do
 * not need to instantiate it directly.
 */
class AppTopBarRegion
{
public:
    explicit AppTopBarRegion(AppPageRoot &page);
    ~AppTopBarRegion();

    void enable();
    void disable();
    void on_screen_deleted();
    void set_page_title(const std::string &title);
    void update_time();
    void update_status();
    void update_battery(const cp0_battery_info_t &battery);

private:
    static void battery_event_cb(lv_event_t *event);
    static void time_timer_cb(lv_timer_t *timer);
    static void status_timer_cb(lv_timer_t *timer);
    void on_top_bar_deleted();

    UIAppTopBar top_bar_;
    AppPageRoot &page_;
    bool enabled_ = false;
    std::unique_ptr<cp0::status::Lifecycle> lifecycle_;
};

/**
 * @brief Standard page layout with a content region and top bar.
 *
 * AppPage is the usual application-facing class. Use this layout directly
 * only when a custom page base is required.
 */
class AppPageLayout : virtual public AppContentRegion
{
public:
    AppPageLayout();
    virtual ~AppPageLayout() = default;
};

/**
 * @brief Default application page base.
 *
 * Inherit from AppPage for a normal page. It provides the root screen, the
 * standard content container (ui_APP_Container), and an enabled top bar.
 * For a completely blank screen with no standard layout, inherit from
 * AppPageRoot instead.
 */
class AppPage : public AppPageLayout
{
public:
    AppPage() = default;
    ~AppPage() override = default;
};

/**
 * @brief Load an already-constructed page's screen and configure input focus.
 * @param page Page object whose screen should become active.
 */
void cp0_lvgl_start_app_page(AppPageRoot &page);

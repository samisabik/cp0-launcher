/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "settings_extras.hpp"
#include "settings_page.hpp"

#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "hal_lvgl_bsp.h"
#include "settings_battery_info_page.hpp"
#include "settings_bluetooth_connected_devices_page.hpp"
#include "settings_bluetooth_page.hpp"
#include "settings_bluetooth_scan_page.hpp"
#include "settings_brightness_page.hpp"
#include "settings_choice_page.hpp"
#include "settings_confirmation_page.hpp"
#include "settings_ethernet_controller.hpp"
#include "settings_menu_roller.hpp"
#include "settings_rtc_page.hpp"
#include "settings_screen_timeout_page.hpp"
#include "settings_submenu_page.hpp"
#include "settings_system_page.hpp"
#include "settings_adapter.hpp"
#include "settings_static_info_page.hpp"
#include "settings_value_page.hpp"
#include "settings_volume_page.hpp"
#include "settings_wifi_page.hpp"
#include "settings_tree_types.hpp"
#include "settings_extport.hpp"
#include "settings_hardware_page.hpp"
#include "settings_power_overlay.hpp"
#include "settings_prefs.hpp"
#include "../model/setup_value_policy.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

namespace {

bool wifi_power_state   = false;
bool wifi_power_pending = false;
std::recursive_mutex wifi_state_mutex;

bool query_wifi_power(bool &enabled)
{
    auto result = std::make_shared<std::atomic<int>>(-1);
    try {
        cp0_signal_wifi_api({"RadioEnabled"},
                            [result](int code, std::string) { result->store(code, std::memory_order_release); });
    } catch (...) {
        return false;
    }

    const int code = result->load(std::memory_order_acquire);
    if (code != 0 && code != 1) return false;
    enabled = code == 1;
    return true;
}

void wifi_power_api(int cmd, void *data)
{
    if (cmd == SettingApiReadFlag && data) {
        {
            std::lock_guard<std::recursive_mutex> lock(wifi_state_mutex);
            if (wifi_power_pending) {
                *static_cast<bool *>(data) = wifi_power_state;
                return;
            }
        }

        bool enabled       = false;
        const bool success = query_wifi_power(enabled);
        std::lock_guard<std::recursive_mutex> lock(wifi_state_mutex);
        if (!wifi_power_pending && success) wifi_power_state = enabled;
        *static_cast<bool *>(data) = wifi_power_state;
    } else if (cmd == SettingApiReadFlagTimeStart && data) {
        auto *result = static_cast<SettingApiReadFlagTimeStartData *>(data);
        {
            std::lock_guard<std::recursive_mutex> lock(wifi_state_mutex);
            if (wifi_power_pending) {
                std::get<0>(*result) = wifi_power_state;
                return;
            }
        }

        bool enabled       = false;
        const bool success = query_wifi_power(enabled);
        std::lock_guard<std::recursive_mutex> lock(wifi_state_mutex);
        if (!wifi_power_pending && success) wifi_power_state = enabled;
        std::get<0>(*result) = wifi_power_state;
    } else if (cmd == SettingApiActivate) {
        bool next = false;
        {
            std::lock_guard<std::recursive_mutex> lock(wifi_state_mutex);
            if (wifi_power_pending) return;
            next               = !wifi_power_state;
            wifi_power_state   = next;
            wifi_power_pending = true;
        }

        try {
            cp0_signal_wifi_api({"RadioSetEnabled", next ? "on" : "off"}, [next](int code, std::string) {
                std::lock_guard<std::recursive_mutex> lock(wifi_state_mutex);
                wifi_power_pending = false;
                if (code != 0) wifi_power_state = !next;
            });
        } catch (...) {
            std::lock_guard<std::recursive_mutex> lock(wifi_state_mutex);
            wifi_power_pending = false;
            wifi_power_state   = !next;
        }
    }
}

settings_ethernet::CommandResult execute_ethernet_operation(settings_ethernet::Operation operation)
{
    struct Invocation {
        std::mutex mutex;
        std::condition_variable condition;
        settings_ethernet::CommandResult result;
        bool completed = false;
    };
    auto invocation = std::make_shared<Invocation>();
    try {
        cp0_signal_process_api(
            settings_ethernet::process_request(operation),
            [invocation](int code, std::string output) {
                {
                    std::lock_guard<std::mutex> lock(invocation->mutex);
                    if (invocation->completed) return;
                    invocation->result.code = code;
                    invocation->result.output = std::move(output);
                    invocation->completed = true;
                }
                invocation->condition.notify_all();
            });
    } catch (...) {
        return {};
    }
    std::unique_lock<std::mutex> lock(invocation->mutex);
    const auto callback_timeout = operation == settings_ethernet::Operation::QueryState
        ? std::chrono::seconds(7)
        : std::chrono::seconds(12);
    if (!invocation->condition.wait_for(lock, callback_timeout,
                                        [&invocation] { return invocation->completed; }))
        return {};
    return invocation->result;
}

settings_ethernet::Controller &ethernet_controller()
{
    static settings_ethernet::Controller controller(execute_ethernet_operation);
    return controller;
}

void ethernet_enabled_api(int cmd, void *data)
{
    if ((cmd == SettingApiReadFlag || cmd == SettingApiReadFlagTimeStart) && data) {
        auto &controller = ethernet_controller();
        auto state = controller.snapshot();
        if (!state.pending) {
            controller.request_refresh();
            state = controller.snapshot();
        }

        if (cmd == SettingApiReadFlag) {
            *static_cast<bool *>(data) = state.known && state.connected;
        } else {
            auto *result = static_cast<SettingApiReadFlagTimeStartData *>(data);
            std::get<0>(*result) = state.known && state.connected;
            if (auto *operation_started = std::get<1>(*result)) {
                operation_started->store(state.pending, std::memory_order_release);
            }
        }
        return;
    }
    if (cmd == SettingApiActivate) ethernet_controller().toggle();
}

Tree *&settings_tree_factory_context()
{
    static Tree *tree = nullptr;
    return tree;
}

// Read-only rows carry their value after a fixed prefix ("ip: 10.0.0.2"); the
// submenu shows the node label, so refreshing a row means rewriting it.
void set_prefixed_label(const NodeIter &page_node, const char *prefix, const std::string &value)
{
    for (auto child = page_node.begin(); child != page_node.end(); ++child) {
        if (child->label.rfind(prefix, 0) == 0) {
            child->label = std::string(prefix) + value;
            return;
        }
    }
}

constexpr const char *kEthernetIpPrefix = "ip: ";
constexpr const char *kTimeNowPrefix = "now: ";
constexpr const char *kDeviceNamePrefix = "device name: ";

void refresh_submenu_labels(const NodeIter &page_node)
{
    if (page_node->label == "ethernet") {
        const std::string ip = settings_interface_ipv4("eth0");
        set_prefixed_label(page_node, kEthernetIpPrefix, ip.empty() ? "--" : ip);
    } else if (page_node->label == "time") {
        set_prefixed_label(page_node, kTimeNowPrefix, settings_prefs::format_now());
    } else if (page_node->label == "system") {
        const std::string host = settings_hostname();
        set_prefixed_label(page_node, kDeviceNamePrefix, host.empty() ? "--" : host);
    }
}

static std::unique_ptr<DComponens::LvglComponensBase> roller_page_factory(lv_obj_t *parent, const NodeIter &page_node,
                                                                          std::function<void()> on_back)
{
#ifdef LAUNCHER_BUILD
    if (page_node->label == "apps") {
        Tree *tree = settings_tree_factory_context();
        if (tree) {
            settings_t12b::populate_launcher_children(*tree, page_node);
        }
    }
#endif
    if (page_node->label == "time") {
        // Refresh before the page is built: the constructor's own status read
        // then sees initialized=true and returns, so one visit costs one D-Bus
        // read instead of two.  The status icon and the Set Manually gate both
        // read the value this leaves behind.
        settings_rtc_refresh_ntp();
    }
    refresh_submenu_labels(page_node);
    return std::make_unique<LvSettingRollerPage2>(parent, page_node, std::move(on_back));
}

static std::unique_ptr<DComponens::LvglComponensBase> roller_page3_factory(lv_obj_t *parent, const NodeIter &page_node,
                                                                           std::function<void()> on_back)
{
    return std::make_unique<LvSettingRollerPage3>(parent, page_node, std::move(on_back));
}

static std::unique_ptr<DComponens::LvglComponensBase> brightness_page3_factory(lv_obj_t *parent,
                                                                               const NodeIter &page_node,
                                                                               std::function<void()> on_back)
{
    return std::make_unique<LvSettingBrightnessPage3>(parent, page_node, std::move(on_back));
}

static std::unique_ptr<DComponens::LvglComponensBase> dark_time_page3_factory(lv_obj_t *parent,
                                                                              const NodeIter &page_node,
                                                                              std::function<void()> on_back)
{
    return std::make_unique<LvSettingDarkTimePage3>(parent, page_node, std::move(on_back));
}

static std::unique_ptr<DComponens::LvglComponensBase> volume_page3_factory(lv_obj_t *parent, const NodeIter &page_node,
                                                                           std::function<void()> on_back)
{
    return std::make_unique<LvSettingVolumePage3>(parent, page_node, std::move(on_back));
}

static std::unique_ptr<DComponens::LvglComponensBase> confirm_page3_factory(lv_obj_t *parent, const NodeIter &page_node,
                                                                            std::function<void()> on_back)
{
    return std::make_unique<LvSettingConfirmPage3>(parent, page_node, std::move(on_back));
}

static std::unique_ptr<DComponens::LvglComponensBase> wifi_scan_page3_factory(lv_obj_t *parent,
                                                                              const NodeIter &page_node,
                                                                              std::function<void()> on_back)
{
    bool enabled = false;
    query_wifi_power(enabled);
    return std::make_unique<LvSettingWifiScanPage3>(parent, page_node, std::move(on_back), false, enabled);
}

static std::unique_ptr<DComponens::LvglComponensBase> bluetooth_connected_page_factory(lv_obj_t *parent,
                                                                                       const NodeIter &page_node,
                                                                                       std::function<void()> on_back)
{
    return std::make_unique<LvSettingBluetoothConnectedPage3>(parent, page_node, std::move(on_back));
}

static std::unique_ptr<DComponens::LvglComponensBase> bluetooth_scan_page_factory(lv_obj_t *parent,
                                                                                  const NodeIter &page_node,
                                                                                  std::function<void()> on_back)
{
    return std::make_unique<LvSettingBluetoothScanPage3>(parent, page_node, std::move(on_back));
}

static std::unique_ptr<DComponens::LvglComponensBase> device_name_page_factory(lv_obj_t *parent,
                                                                               const NodeIter &page_node,
                                                                               std::function<void()> on_back)
{
    return std::make_unique<LvSettingBluetoothAliasPage3>(
        parent, page_node, std::move(on_back), settings_hostname(),
        [page_node](std::string value) { page_node->label = kDeviceNamePrefix + std::move(value); },
        [](const std::string &name) { return settings_run_admin({"HostnameSet", name}); });
}

bool bluetooth_power_state   = false;
bool bluetooth_power_pending = false;
std::recursive_mutex bluetooth_state_mutex;

bool query_bluetooth_power(bool &powered)
{
    bool success = false;
    try {
        cp0_signal_bt_api({"BtStatus"}, [&](int code, std::string data) {
            std::istringstream input(data);
            std::string powered_text;
            if (code != 0 || !std::getline(input, powered_text, '\t')) return;
            if (powered_text != "0" && powered_text != "1") return;
            powered = powered_text == "1";
            success = true;
        });
    } catch (...) {
    }
    return success;
}

void bluetooth_power_api(int cmd, void *data)
{
    std::lock_guard<std::recursive_mutex> state_lock(bluetooth_state_mutex);
    if ((cmd == SettingApiReadFlag || cmd == SettingApiReadFlagTimeStart) && data) {
        bool powered = bluetooth_power_state;
        if (!bluetooth_power_pending && query_bluetooth_power(powered)) bluetooth_power_state = powered;
        if (cmd == SettingApiReadFlag)
            *static_cast<bool *>(data) = bluetooth_power_state;
        else
            std::get<0>(*static_cast<SettingApiReadFlagTimeStartData *>(data)) = bluetooth_power_state;
        return;
    }
    if (cmd != SettingApiActivate || bluetooth_power_pending) return;

    const bool next = !bluetooth_power_state;
    bluetooth_power_pending = true;
    bluetooth_power_state   = next;
    try {
        cp0_signal_bt_api({"BtPower", next ? "1" : "0"}, [next](int code, std::string) {
            std::lock_guard<std::recursive_mutex> lock(bluetooth_state_mutex);
            bluetooth_power_pending = false;
            if (code != 0) bluetooth_power_state = !next;
        });
    } catch (...) {
        bluetooth_power_pending = false;
        bluetooth_power_state   = !next;
    }
}

static void append_brightness_options(Tree &tree, const NodeIter &parent)
{
    for (int index = 0; index < setup_values::kBrightnessStepCount; ++index) {
        tree.append_child(parent, SettingEntry{
            std::to_string(setup_values::brightness_step_percent(index)) + "%"});
    }
}

// Confirmation page with yes/no; "yes" hands the screen to the power notice.
void append_power_action(Tree &tree, const NodeIter &parent, settings_t12b::boot_actions::Action action)
{
    const auto &view = settings_t12b::boot_actions::presentation(action);
    const NodeIter node = tree.append_child(parent, SettingEntry{std::string(view.label), confirm_page3_factory});
    tree.append_child(node, SettingEntry{"yes",
                                         [action](int command, void *) {
                                             if (command == SettingApiActivate) settings_power_request(action);
                                         },
                                         false});
    tree.append_child(node, SettingEntry{"no", [](int, void *) {}, false});
}

}  // namespace

void UISettingTreePage::create_page_detail()
{
    Tree &mode_tree                 = mode_tree_;
    settings_tree_factory_context() = &mode_tree;
    NodeIter root                   = mode_tree.set_head(SettingEntry{"Settings"});

    {
        NodeIter screen = mode_tree.append_child(root, SettingEntry{"screen", roller_page_factory});
        {
            NodeIter brightness = mode_tree.append_child(screen, SettingEntry{"brightness", brightness_page3_factory});
            append_brightness_options(mode_tree, brightness);
        }
        {
            NodeIter dark_time = mode_tree.append_child(screen, SettingEntry{"auto-lock", dark_time_page3_factory});
            mode_tree.append_child(dark_time, SettingEntry{"never"});
            mode_tree.append_child(dark_time, SettingEntry{"10 s"});
            mode_tree.append_child(dark_time, SettingEntry{"30 s"});
            mode_tree.append_child(dark_time, SettingEntry{"1 min"});
            mode_tree.append_child(dark_time, SettingEntry{"5 min"});
        }
    }

    {
        NodeIter sound = mode_tree.append_child(root, SettingEntry{"sound", roller_page_factory});
        NodeIter volume = mode_tree.append_child(
            sound, SettingEntry{"volume", volume_page3_factory, PageType::FullCustom});
        for (int index = 0; index < setup_values::volume_metric(setup_values::VolumeMetric::OptionCount); ++index) {
            mode_tree.append_child(
                volume, SettingEntry{std::to_string(setup_values::volume_percent(index)) + "%"});
        }
        NodeIter mic_gain = mode_tree.append_child(sound, SettingEntry{"mic gain", settings_mic_gain_page_factory});
        append_mic_gain_options(mode_tree, mic_gain);
    }

    {
        NodeIter wifi = mode_tree.append_child(root, SettingEntry{"wi-fi", roller_page_factory});
        mode_tree.append_child(wifi, SettingEntry{"enable", wifi_power_api, true});
        mode_tree.append_child(wifi, SettingEntry{"networks", wifi_scan_page3_factory, PageType::FullCustom});
    }

    {
        NodeIter bluetooth = mode_tree.append_child(root, SettingEntry{"bluetooth", roller_page_factory});
        mode_tree.append_child(bluetooth, SettingEntry{"power", bluetooth_power_api, true});
        mode_tree.append_child(bluetooth,
                               SettingEntry{"paired", bluetooth_connected_page_factory, PageType::FullCustom});
        mode_tree.append_child(bluetooth, SettingEntry{"scan", bluetooth_scan_page_factory, PageType::FullCustom});
    }

    {
        NodeIter ethernet = mode_tree.append_child(root, SettingEntry{"ethernet", roller_page_factory});
        NodeIter ethernet_enable =
            mode_tree.append_child(ethernet, SettingEntry{"enable", ethernet_enabled_api, true});
        ethernet_enable->status_read_policy = SettingStatusReadPolicy::Direct;
        mode_tree.append_child(ethernet, SettingEntry{std::string(kEthernetIpPrefix) + "--"});
    }

    {
        NodeIter tailscale = mode_tree.append_child(root, SettingEntry{"tailscale", roller_page_factory});
        mode_tree.append_child(tailscale, SettingEntry{"enable", tailscale_enabled_api, true});
        mode_tree.append_child(
            tailscale, SettingEntry{"status", settings_tailscale_page_factory, PageType::FullCustom});
    }

    {
        NodeIter ext_port = mode_tree.append_child(root, SettingEntry{"ext port", roller_page_factory});
        mode_tree.append_child(
            ext_port,
            SettingEntry{"ext 5v", std::bind(&ext_port_com, "EXT5V", std::placeholders::_1,
                                              std::placeholders::_2), true});
        mode_tree.append_child(
            ext_port,
            SettingEntry{"grove 5v", std::bind(&ext_port_com, "GROVE5V", std::placeholders::_1,
                                                std::placeholders::_2), true});
    }

    mode_tree.append_child(root, SettingEntry{"battery", settings_battery_info_page_factory, PageType::FullCustom});

#ifdef LAUNCHER_BUILD
    mode_tree.append_child(root, SettingEntry{"apps", roller_page_factory});
#endif

    {
        NodeIter date_time = mode_tree.append_child(root, SettingEntry{"time", roller_page_factory});
        mode_tree.append_child(date_time, SettingEntry{std::string(kTimeNowPrefix) + "--:--"});
        SettingEntry ntp_entry{"network time", settings_rtc_ntp_api, true};
        ntp_entry.status_read_policy = SettingStatusReadPolicy::Direct;
        mode_tree.append_child(date_time, std::move(ntp_entry));
        NodeIter zone = mode_tree.append_child(date_time, SettingEntry{"time zone", settings_time_zone_page_factory});
        append_time_zone_options(mode_tree, zone);
        NodeIter clock_format =
            mode_tree.append_child(date_time, SettingEntry{"clock format", settings_clock_format_page_factory});
        append_clock_format_options(mode_tree, clock_format);
        SettingEntry manual_entry{"set manually", settings_rtc_editor_page_factory, PageType::FullCustom};
        // While Network Time keeps the clock there is nothing to set by hand, so
        // refuse the page up front and say why instead of letting the write fail.
        manual_entry.activation_gate = [] { return settings_rtc_manual_edit_block(); };
        mode_tree.append_child(date_time, std::move(manual_entry));
    }

    {
        NodeIter system = mode_tree.append_child(root, SettingEntry{"system", roller_page_factory});
        mode_tree.append_child(system,
                               SettingEntry{"about", settings_t12b_about_page_factory, PageType::FullCustom});
        mode_tree.append_child(system, SettingEntry{std::string(kDeviceNamePrefix) + "--", device_name_page_factory,
                                                    PageType::FullCustom});
        mode_tree.append_child(system,
                               SettingEntry{"hardware", settings_hardware_page_factory, PageType::FullCustom});
        SettingEntry ssh_entry{"ssh server", ssh_server_api, true};
        ssh_entry.status_read_policy = SettingStatusReadPolicy::Direct;
        mode_tree.append_child(system, std::move(ssh_entry));
        mode_tree.append_child(system,
                               SettingEntry{"updates", settings_updates_page_factory, PageType::FullCustom});
        append_power_action(mode_tree, system, settings_t12b::boot_actions::Action::Reboot);
        append_power_action(mode_tree, system, settings_t12b::boot_actions::Action::Shutdown);
    }

}

void UISettingTreePage::back_home(void *data)
{
    auto *page = static_cast<UISettingTreePage *>(data);
    if (!page) return;

    page->AnimateNextOut(nullptr);
    page->roller_.reset();
    if (page->navigate_home) page->navigate_home();
}

void UISettingTreePage::LeaveNextPage()
{
    if (lv_async_call(back_home, this) != LV_RESULT_OK) back_home(this);
}

void UISettingTreePage::AnimateNextIn(std::function<void()> animate_over_func)
{
    if (animate_over_func) animate_over_func();
}

void UISettingTreePage::AnimateNextOut(std::function<void()> animate_over_func)
{
    if (animate_over_func) animate_over_func();
}

void UISettingTreePage::LoadNextPage()
{
    roller_ = std::make_unique<LvSettingRoller>(ui_APP_Container, mode_tree_.begin(),
                                                std::bind(&UISettingTreePage::LeaveNextPage, this));
    AnimateNextIn([this]() {
        if (!roller_) return;
        lv_group_add_obj(input_group(), roller_->Get());
        lv_group_focus_obj(roller_->Get());
    });
}

UISettingTreePage::UISettingTreePage() : AppPage()
{
    set_page_title("Settings");
    reclaim_top_bar();
    lv_obj_set_style_bg_color(screen(), lv_color_black(), LV_PART_MAIN);
    cp0::ui::paint_background(ui_APP_Container);
    // The frame stays put while the menu columns slide; see
    // LvSettingRollerPage2::animate_first_page_objects().
    lv_obj_t *frame = cp0::ui::box(ui_APP_Container, 0, 0, cp0::theme::kScreenW, cp0::theme::kScreenH, cp0::theme::kBg);
    if (frame) {
        lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(frame, LV_OBJ_FLAG_USER_1);
        cp0::ui::title_row(frame, "settings");
        cp0::ui::set_footer(cp0::ui::footer(frame), {{"up/dn", "move"}, {"ent", "open"}, {"esc", "back"}});
    }
    create_page_detail();
    LoadNextPage();
}

UISettingTreePage::~UISettingTreePage()
{
    lv_async_call_cancel(back_home, this);
    roller_.reset();
    if (settings_tree_factory_context() == &mode_tree_) settings_tree_factory_context() = nullptr;
}

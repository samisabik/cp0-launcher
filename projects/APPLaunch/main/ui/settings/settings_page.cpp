/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "settings_extras.hpp"
#include "settings_page.hpp"

#include "cp0_lvgl_app.h"
#include "hal_lvgl_bsp.h"
#include "settings_battery_info_page.hpp"
#include "settings_bluetooth_connected_devices_page.hpp"
#include "settings_bluetooth_scan_page.hpp"
#include "settings_brightness_page.hpp"
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

bool query_bluetooth_status(bool &powered, bool &discoverable, std::string *alias_output = nullptr);

void refresh_bluetooth_alias(const NodeIter &page_node)
{
    bool powered = false;
    bool discoverable = false;
    std::string alias;
    if (!query_bluetooth_status(powered, discoverable, &alias) || alias.empty()) return;

    constexpr const char *prefix = "alias: ";
    for (auto child = page_node.begin(); child != page_node.end(); ++child) {
        if (child->label.rfind(prefix, 0) == 0) {
            child->label = std::string(prefix) + alias;
            break;
        }
    }
}

static std::unique_ptr<DComponens::LvglComponensBase> bluetooth_roller_page_factory(lv_obj_t *parent,
                                                                                    const NodeIter &page_node,
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
    refresh_bluetooth_alias(page_node);
    return std::make_unique<LvSettingRollerPage2>(parent, page_node, std::move(on_back));
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
    auto page = std::make_unique<LvSettingRollerPage2>(parent, page_node, std::move(on_back));
    if (page_node->label == "time") {
        // Manual time edits belong to a single visit to Date & Time; leaving the
        // submenu abandons them so a later visit cannot write a stale time.
        page->set_on_destroy([] { settings_rtc_discard_edits(); });
    }
    return page;
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

static std::unique_ptr<DComponens::LvglComponensBase> rtc_page3_factory(lv_obj_t *parent, const NodeIter &page_node,
                                                                        std::function<void()> on_back)
{
    // "day" is declared with a static 1..31 option list, but the model clamps
    // the day to the month that is actually selected.  Rebuild the options from
    // the model so the page can never offer a date that would be rejected on
    // commit.  This runs before the page object is constructed and after the
    // previous third-level page (with its per-entry action backups) has been
    // destroyed, so nothing holds an iterator into the children being replaced.
    if (page_node->label == "day") {
        if (Tree *tree = settings_tree_factory_context()) {
            // Safe to replace the children here: LoadNextPage() returns early
            // while roller3_ is set, so the previous third-level page has been
            // destroyed - and its restore_actions() has run - before this
            // factory is called.  No live SettingEntry* points into these
            // children; dropping that guard would turn this into a
            // use-after-free.
            tree->erase_children(page_node);
            const int days = settings_rtc_days_in_current_month();
            // The model keeps the month within 1..12, so this is 28..31; clamp
            // so a bad value cannot build a page with no rows at all.
            const int last_day = days >= 1 ? days : 31;
            for (int day = 1; day <= last_day; ++day) {
                tree->append_child(page_node, SettingEntry{std::to_string(day)});
            }
        }
    }
    return std::make_unique<LvSettingRtcPage3>(parent, page_node, std::move(on_back));
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

static std::unique_ptr<DComponens::LvglComponensBase> wifi_add_hidden_page_factory(lv_obj_t *parent,
                                                                                   const NodeIter &page_node,
                                                                                   std::function<void()> on_back)
{
    bool enabled = false;
    query_wifi_power(enabled);
    return std::make_unique<LvSettingWifiScanPage3>(parent, page_node, std::move(on_back), true, enabled);
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

static std::unique_ptr<DComponens::LvglComponensBase> bluetooth_alias_page_factory(lv_obj_t *parent,
                                                                                   const NodeIter &page_node,
                                                                                   std::function<void()> on_back)
{
    std::string alias            = page_node->label;
    constexpr const char *prefix = "alias: ";
    if (alias.rfind(prefix, 0) == 0) alias.erase(0, std::strlen(prefix));
    return std::make_unique<LvSettingBluetoothAliasPage3>(
        parent, page_node, std::move(on_back), std::move(alias),
        [page_node](std::string value) { page_node->label = "alias: " + std::move(value); });
}

bool bluetooth_power_state          = false;
bool bluetooth_discoverable_state   = false;
bool bluetooth_named_only_state     = true;
bool bluetooth_power_pending        = false;
bool bluetooth_discoverable_pending = false;
std::recursive_mutex bluetooth_state_mutex;

constexpr const char *kBluetoothNamedOnlyConfigKey = "bt_named_only";

bool read_bluetooth_named_only_config(bool fallback)
{
    int value = fallback ? 1 : 0;
    try {
        cp0_signal_config_api(
            {"GetInt", kBluetoothNamedOnlyConfigKey, "1"},
            [&](int code, std::string data) {
                int parsed = 0;
                const char *begin = data.data();
                const char *end = begin + data.size();
                const auto result = std::from_chars(begin, end, parsed);
                if (code == 0 && result.ec == std::errc{} && result.ptr == end &&
                    (parsed == 0 || parsed == 1))
                    value = parsed != 0 ? 1 : 0;
            });
    } catch (...) {
    }
    return value != 0;
}

bool write_bluetooth_named_only_config(int value)
{
    bool succeeded = false;
    try {
        cp0_signal_config_api(
            {"SetInt", kBluetoothNamedOnlyConfigKey, value != 0 ? "1" : "0"},
            [&](int code, std::string) { succeeded = code == 0; });
    } catch (...) {
    }
    return succeeded;
}

bool save_bluetooth_config()
{
    bool succeeded = false;
    try {
        cp0_signal_config_api({"Save"}, [&](int code, std::string) { succeeded = code == 0; });
    } catch (...) {
    }
    return succeeded;
}

bool query_bluetooth_status(bool &powered, bool &discoverable, std::string *alias_output)
{
    bool success = false;
    try {
        cp0_signal_bt_api({"BtStatus"}, [&](int code, std::string data) {
            std::istringstream input(data);
            std::string powered_text;
            std::string address;
            std::string discoverable_text;
            std::string alias;
            if (code != 0 || !std::getline(input, powered_text, '\t') || !std::getline(input, address, '\t') ||
                !std::getline(input, discoverable_text, '\t') || !std::getline(input, alias, '\t'))
                return;
            if ((powered_text != "0" && powered_text != "1") || (discoverable_text != "0" && discoverable_text != "1"))
                return;
            powered      = powered_text == "1";
            discoverable = discoverable_text == "1";
            if (alias_output) *alias_output = alias;
            success      = true;
        });
    } catch (...) {
    }
    return success;
}

void bluetooth_toggle_api(int cmd, void *data, bool &state, const char *command)
{
    std::lock_guard<std::recursive_mutex> state_lock(bluetooth_state_mutex);
    if (cmd == SettingApiReadFlag && data) {
        bool powered      = bluetooth_power_state;
        bool discoverable = bluetooth_discoverable_state;
        if (command &&
            !((std::strcmp(command, "BtPower") == 0 && bluetooth_power_pending) ||
              (std::strcmp(command, "BtDiscoverable") == 0 && bluetooth_discoverable_pending)) &&
            query_bluetooth_status(powered, discoverable)) {
            state = std::strcmp(command, "BtDiscoverable") == 0 ? discoverable : powered;
            if (std::strcmp(command, "BtPower") == 0)
                bluetooth_power_state = powered;
            else if (std::strcmp(command, "BtDiscoverable") == 0)
                bluetooth_discoverable_state = discoverable;
        }
        *static_cast<bool *>(data) = state;
    } else if (cmd == SettingApiReadFlagTimeStart && data) {
        auto *result      = static_cast<SettingApiReadFlagTimeStartData *>(data);
        bool powered      = bluetooth_power_state;
        bool discoverable = bluetooth_discoverable_state;
        if (command &&
            !((std::strcmp(command, "BtPower") == 0 && bluetooth_power_pending) ||
              (std::strcmp(command, "BtDiscoverable") == 0 && bluetooth_discoverable_pending)) &&
            query_bluetooth_status(powered, discoverable)) {
            state = std::strcmp(command, "BtDiscoverable") == 0 ? discoverable : powered;
            if (std::strcmp(command, "BtPower") == 0)
                bluetooth_power_state = powered;
            else if (std::strcmp(command, "BtDiscoverable") == 0)
                bluetooth_discoverable_state = discoverable;
        }
        std::get<0>(*result) = state;
    } else if (cmd == SettingApiActivate) {
        if (command && ((std::strcmp(command, "BtPower") == 0 && bluetooth_power_pending) ||
                        (std::strcmp(command, "BtDiscoverable") == 0 && bluetooth_discoverable_pending)))
            return;

        const bool next = !state;
        if (command) {
            try {
                if (std::strcmp(command, "BtPower") == 0) bluetooth_power_pending = true;
                if (std::strcmp(command, "BtDiscoverable") == 0) bluetooth_discoverable_pending = true;

                state = next;
                cp0_signal_bt_api({command, next ? "1" : "0"}, [command, next](int code, std::string) {
                    std::lock_guard<std::recursive_mutex> state_lock(bluetooth_state_mutex);
                    if (std::strcmp(command, "BtPower") == 0) {
                        bluetooth_power_pending = false;
                        if (code != 0) bluetooth_power_state = !next;
                    } else if (std::strcmp(command, "BtDiscoverable") == 0) {
                        bluetooth_discoverable_pending = false;
                        if (code != 0) bluetooth_discoverable_state = !next;
                    }
                });
            } catch (...) {
                if (std::strcmp(command, "BtPower") == 0) {
                    bluetooth_power_pending = false;
                    bluetooth_power_state   = !next;
                } else if (std::strcmp(command, "BtDiscoverable") == 0) {
                    bluetooth_discoverable_pending = false;
                    bluetooth_discoverable_state   = !next;
                }
            }
        } else {
            state = next;
        }
    }
}

void bluetooth_power_api(int cmd, void *data)
{
    bluetooth_toggle_api(cmd, data, bluetooth_power_state, "BtPower");
}

void bluetooth_discoverable_api(int cmd, void *data)
{
    if (cmd == SettingApiActivate) {
        auto *page = static_cast<LvSettingRollerPage2 *>(data);
        std::lock_guard<std::recursive_mutex> state_lock(bluetooth_state_mutex);
        if (bluetooth_discoverable_pending) return;

        bool powered              = bluetooth_power_state;
        bool status_powered       = powered;
        bool ignored_discoverable = false;
        if (!bluetooth_power_pending && query_bluetooth_status(status_powered, ignored_discoverable)) {
            powered = status_powered;
        }
        if (bluetooth_power_pending || !powered) {
            if (page) page->show_power_warning();
            return;
        }
    }
    bluetooth_toggle_api(cmd, data, bluetooth_discoverable_state, "BtDiscoverable");
}

void bluetooth_named_only_api(int cmd, void *data)
{
    std::lock_guard<std::recursive_mutex> state_lock(bluetooth_state_mutex);
    if ((cmd == SettingApiReadFlag || cmd == SettingApiReadFlagTimeStart) && data) {
        bluetooth_named_only_state =
            read_bluetooth_named_only_config(bluetooth_named_only_state);
        if (cmd == SettingApiReadFlag)
            *static_cast<bool *>(data) = bluetooth_named_only_state;
        else
            std::get<0>(*static_cast<SettingApiReadFlagTimeStartData *>(data)) =
                bluetooth_named_only_state;
        return;
    }
    if (cmd != SettingApiActivate) return;

    const bool previous =
        read_bluetooth_named_only_config(bluetooth_named_only_state);
    const bool desired = !previous;
    if (!write_bluetooth_named_only_config(desired ? 1 : 0)) {
        bluetooth_named_only_state = previous;
        return;
    }
    if (save_bluetooth_config()) {
        bluetooth_named_only_state = desired;
        return;
    }

    // SetInt mutates the live config immediately. Restore it when Save fails
    // so a later page rebuild cannot observe an unpersisted value.
    if (write_bluetooth_named_only_config(previous ? 1 : 0))
        (void)save_bluetooth_config();
    bluetooth_named_only_state = previous;
}

static void append_numeric_options(Tree &tree, const NodeIter &parent, int first, int last)
{
    for (int value = first; value <= last; ++value) tree.append_child(parent, SettingEntry{std::to_string(value)});
}

static void append_brightness_options(Tree &tree, const NodeIter &parent)
{
    for (int index = 0; index < setup_values::kBrightnessStepCount; ++index) {
        tree.append_child(parent, SettingEntry{
            std::to_string(setup_values::brightness_step_percent(index)) + "%"});
    }
}

}  // namespace

bool settings_bluetooth_named_only_enabled()
{
    std::lock_guard<std::recursive_mutex> state_lock(bluetooth_state_mutex);
    bluetooth_named_only_state =
        read_bluetooth_named_only_config(bluetooth_named_only_state);
    return bluetooth_named_only_state;
}

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
        mode_tree.append_child(wifi, SettingEntry{"hidden", wifi_add_hidden_page_factory, PageType::FullCustom});
    }

    {
        NodeIter bluetooth = mode_tree.append_child(root, SettingEntry{"bluetooth", bluetooth_roller_page_factory});
        mode_tree.append_child(bluetooth, SettingEntry{"power", bluetooth_power_api, true});
        mode_tree.append_child(bluetooth, SettingEntry{"alias: ", bluetooth_alias_page_factory});
        mode_tree.append_child(bluetooth, SettingEntry{"visible", bluetooth_discoverable_api, true});
        mode_tree.append_child(bluetooth, SettingEntry{"named only", bluetooth_named_only_api, true});
        mode_tree.append_child(bluetooth,
                               SettingEntry{"paired", bluetooth_connected_page_factory, PageType::FullCustom});
        mode_tree.append_child(bluetooth, SettingEntry{"scan", bluetooth_scan_page_factory, PageType::FullCustom});
    }

    {
        NodeIter ethernet = mode_tree.append_child(root, SettingEntry{"ethernet", roller_page_factory});
        NodeIter ethernet_enable =
            mode_tree.append_child(ethernet, SettingEntry{"enable", ethernet_enabled_api, true});
        ethernet_enable->status_read_policy = SettingStatusReadPolicy::Direct;
        mode_tree.append_child(
            ethernet, SettingEntry{"info", settings_ethernet_page_factory, PageType::FullCustom});
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
        mode_tree.append_child(date_time,
                               SettingEntry{"info", settings_rtc_info_page_factory, PageType::FullCustom});
        SettingEntry ntp_entry{"network time", settings_rtc_ntp_api, true};
        ntp_entry.status_read_policy = SettingStatusReadPolicy::Direct;
        mode_tree.append_child(date_time, std::move(ntp_entry));
        NodeIter zone = mode_tree.append_child(date_time, SettingEntry{"time zone", settings_time_zone_page_factory});
        append_time_zone_options(mode_tree, zone);
        SettingEntry manual_entry{"set manually", roller_page_factory};
        // While Network Time keeps the clock there is nothing to set by hand, so
        // refuse the page up front and say why instead of letting every edit
        // fail one by one.
        manual_entry.activation_gate = [] { return settings_rtc_manual_edit_block(); };
        NodeIter manual = mode_tree.append_child(date_time, std::move(manual_entry));
        {
            NodeIter year = mode_tree.append_child(manual, SettingEntry{"year", rtc_page3_factory});
            append_numeric_options(mode_tree, year, 2000, 2099);
        }
        {
            NodeIter month = mode_tree.append_child(manual, SettingEntry{"month", rtc_page3_factory});
            append_numeric_options(mode_tree, month, 1, 12);
        }
        {
            NodeIter day = mode_tree.append_child(manual, SettingEntry{"day", rtc_page3_factory});
            append_numeric_options(mode_tree, day, 1, 31);
        }
        {
            NodeIter hour = mode_tree.append_child(manual, SettingEntry{"hour", rtc_page3_factory});
            append_numeric_options(mode_tree, hour, 0, 23);
        }
        {
            NodeIter minute = mode_tree.append_child(manual, SettingEntry{"minute", rtc_page3_factory});
            append_numeric_options(mode_tree, minute, 0, 59);
        }
        {
            NodeIter write_rtc = mode_tree.append_child(
                manual, SettingEntry{"write rtc?", settings_rtc_confirm_page_factory});
            mode_tree.append_child(write_rtc, SettingEntry{"yes"});
            mode_tree.append_child(write_rtc, SettingEntry{"no"});
        }
    }

    {
        NodeIter system = mode_tree.append_child(root, SettingEntry{"system", roller_page_factory});
        mode_tree.append_child(system,
                               SettingEntry{"about", settings_t12b_about_page_factory, PageType::FullCustom});
        mode_tree.append_child(system,
                               SettingEntry{"storage", settings_storage_page_factory, PageType::FullCustom});
        mode_tree.append_child(system,
                               SettingEntry{"licenses", settings_credit_page_factory, PageType::FullCustom});
        settings_t12b::append_boot_action_child(
            mode_tree, system, settings_t12b::boot_actions::Action::Reboot, confirm_page3_factory);
        settings_t12b::append_boot_action_child(
            mode_tree, system, settings_t12b::boot_actions::Action::Shutdown, confirm_page3_factory);
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
    lv_obj_set_style_bg_color(screen(), lv_color_black(), LV_PART_MAIN);
    create_page_detail();
    LoadNextPage();
}

UISettingTreePage::~UISettingTreePage()
{
    lv_async_call_cancel(back_home, this);
    roller_.reset();
    if (settings_tree_factory_context() == &mode_tree_) settings_tree_factory_context() = nullptr;
}

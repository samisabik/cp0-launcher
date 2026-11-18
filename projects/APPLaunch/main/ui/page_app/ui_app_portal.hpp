/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "../model/portal_model.hpp"
#include "cp0_theme.hpp"

#include <cstdint>
#include <string>

struct key_item;

// ============================================================
//  Portal  UIPortalPage
//  Stands up an open test access point on wlan1: hostapd, dnsmasq (DHCP plus
//  a wildcard DNS so captive-portal detection fires) and a static "hello"
//  page. wlan0 stays the system Wi-Fi. The page starts and stops the AP and
//  shows its state and connected-client count; it never collects anything a
//  client sends.
//
//  Opening the app prompts for the access point's name (unless one is already
//  running), pre-filled with the last name used.
//
//  Keys:
//    naming:  type a name, ENTER saves it, ESC home
//    running: ENTER on/off, E rename (when off), ESC home
// ============================================================
class UIPortalPage : public AppPage
{
public:
    UIPortalPage();
    ~UIPortalPage() override;

private:
    enum class State { Off, Starting, On, Stopping, Error };
    enum class View { Naming, Control };

    bool tools_ok_ = false;
    std::string dir_;
    std::string ssid_ = portal::kDefaultSsid;
    int channel_ = portal::kDefaultChannel;

    View view_ = View::Naming;
    std::string name_buf_;
    bool input_context_saved_ = false;
    int previous_input_context_ = 0;
    int previous_keypad_intercept_ = 0;

    State state_ = State::Off;
    int job_pid_ = -1;
    std::string log_path_;
    std::string error_;
    int clients_ = 0;
    uint32_t last_poll_ = 0;

    lv_obj_t *status_ = nullptr;
    lv_obj_t *big_ = nullptr;
    lv_obj_t *sub_ = nullptr;
    lv_obj_t *info_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    void create_ui();
    void load_config();
    void enter_name_edit();
    void commit_name(bool save);
    void restore_input_context();
    bool write_files();
    bool ap_running() const;
    void start_ap();
    void stop_ap();
    void poll_job();
    void refresh_clients();
    void render();
    void on_timer();
    void handle_naming_key(uint32_t key, const key_item *item);
    void handle_control_key(uint32_t key);
    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
};

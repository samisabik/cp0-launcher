/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT

#include "ui_app_portal.hpp"

#include "../app_support.hpp"
#include "../keyboard_text_input.hpp"
#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "input_keys.h"
#include "keyboard_input.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr uint32_t kRefreshMs = 500;

bool write_file(const std::string &path, const std::string &contents)
{
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << contents;
    return out.good();
}

// The page the portal serves: the bundled share/images/portal/index.html,
// or the model's plain fallback when that resource is missing. The launcher's
// path resolver only maps image/audio/font names, not .html, so read the
// bundled page from its known locations: the device install first, then the
// SDL-relative resource tree.
std::string served_page()
{
    for (const char *path : {"/usr/share/APPLaunch/share/images/portal/index.html",
                             "APPLaunch/share/images/portal/index.html"}) {
        const std::string page = app_support::read_file(path);
        if (!page.empty()) return page;
    }
    return portal::index_html();
}

// A pid file holds a process that is still alive.
bool pid_alive(const std::string &pid_path)
{
    const std::string text = app_support::read_file(pid_path);
    const int pid = std::atoi(text.c_str());
    if (pid <= 0) return false;
    return access(("/proc/" + std::to_string(pid)).c_str(), F_OK) == 0;
}

} // namespace

UIPortalPage::UIPortalPage() : AppPage()
{
    set_page_title("Portal");
    reclaim_top_bar();
    dir_ = app_support::home_dir() + "/portal";
    log_path_ = dir_ + "/run.log";
    load_config();
    tools_ok_ = app_support::tool_present("hostapd") && app_support::tool_present("dnsmasq") &&
                app_support::tool_present("python3");
    if (ap_running()) {
        // Reopened while the AP is up: show control, do not rename it live.
        state_ = State::On;
        view_ = View::Control;
    }
    create_ui();
    if (view_ == View::Naming) enter_name_edit();
    refresh_clients();
    render();
    timer_ = lv_timer_create(timer_cb, kRefreshMs, this);
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

UIPortalPage::~UIPortalPage()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
    restore_input_context();
    // The access point keeps running when the page is left, like a service.
    app_support::stop(job_pid_);
}

void UIPortalPage::enter_name_edit()
{
    view_ = View::Naming;
    name_buf_ = ssid_;
    if (!input_context_saved_) {
        previous_input_context_ = cp0_keyboard_get_input_context();
        previous_keypad_intercept_ = cp0_keyboard_get_lvgl_keypad_intercept();
        input_context_saved_ = true;
    }
    cp0_keyboard_set_input_context(KBD_INPUT_CONTEXT_TEXT);
    cp0_keyboard_set_lvgl_keypad_intercept(1);
}

void UIPortalPage::restore_input_context()
{
    if (!input_context_saved_) return;
    cp0_keyboard_set_input_context(static_cast<cp0_keyboard_input_context_t>(previous_input_context_));
    cp0_keyboard_set_lvgl_keypad_intercept(previous_keypad_intercept_);
    input_context_saved_ = false;
}

void UIPortalPage::commit_name(bool save)
{
    restore_input_context();
    view_ = View::Control;
    if (save) {
        std::string name = name_buf_;
        const std::size_t end = name.find_last_not_of(' ');
        name = end == std::string::npos ? std::string() : name.substr(0, end + 1);
        const std::size_t start = name.find_first_not_of(' ');
        if (start != std::string::npos) name = name.substr(start);
        ssid_ = name.empty() ? std::string(portal::kDefaultSsid) : name;
        cp0_signal_config_api({"SetStr", "portal_ssid", ssid_}, nullptr);
        cp0_signal_config_api({"Save"}, nullptr);
    }
    render();
}

void UIPortalPage::load_config()
{
    cp0_signal_config_api({"GetStr", "portal_ssid", portal::kDefaultSsid}, [&](int code, std::string data) {
        if (code == 0 && !data.empty()) ssid_ = data;
    });
    cp0_signal_config_api({"GetInt", "portal_channel", std::to_string(portal::kDefaultChannel)},
                          [&](int code, std::string data) {
                              if (code == 0) channel_ = std::atoi(data.c_str());
                          });
}

void UIPortalPage::create_ui()
{
    if (!ui_APP_Container) return;
    cp0::ui::paint_background(ui_APP_Container);
    const cp0::ui::TitleRow title = cp0::ui::title_row(ui_APP_Container, "portal");
    status_ = title.status;

    cp0::ui::panel(ui_APP_Container, 4, 19, 312, 60);
    big_ = cp0::ui::label(ui_APP_Container, 14, 30, cp0::theme::kText, true);
    sub_ = cp0::ui::label(ui_APP_Container, 14, 58, cp0::theme::kIdle);
    if (sub_) {
        lv_obj_set_width(sub_, 296);
        lv_label_set_long_mode(sub_, LV_LABEL_LONG_CLIP);
    }

    info_ = cp0::ui::label(ui_APP_Container, 14, 88, cp0::theme::kDim);
    if (info_) lv_label_set_long_mode(info_, LV_LABEL_LONG_WRAP), lv_obj_set_width(info_, 296);

    hint_ = cp0::ui::footer(ui_APP_Container);
}

bool UIPortalPage::ap_running() const
{
    return pid_alive(dir_ + "/hostapd.pid");
}

bool UIPortalPage::write_files()
{
    if (mkdir(dir_.c_str(), 0755) != 0 && errno != EEXIST) return false;
    return write_file(dir_ + "/hostapd.conf", portal::hostapd_conf(ssid_, channel_)) &&
           write_file(dir_ + "/dnsmasq.conf", portal::dnsmasq_conf(dir_)) &&
           write_file(dir_ + "/index.html", served_page()) &&
           write_file(dir_ + "/serve.py", portal::serve_py(dir_)) &&
           write_file(dir_ + "/start.sh", portal::start_script(dir_)) &&
           write_file(dir_ + "/stop.sh", portal::stop_script(dir_));
}

void UIPortalPage::start_ap()
{
    if (state_ == State::Starting || state_ == State::Stopping) return;
    if (!tools_ok_) {
        error_ = "hostapd, dnsmasq or python3 missing";
        state_ = State::Error;
        render();
        return;
    }
    if (!write_files()) {
        error_ = "cannot write ~/portal";
        state_ = State::Error;
        render();
        return;
    }
    error_.clear();
    unlink(log_path_.c_str());
    state_ = State::Starting;
    last_poll_ = lv_tick_get();
    job_pid_ = app_support::spawn("( sudo -n sh '" + dir_ + "/start.sh' ) >'" + log_path_ + "' 2>&1; echo \"#rc $?\" >>'" +
                                  log_path_ + "'");
    if (job_pid_ <= 0) {
        error_ = "could not launch";
        state_ = State::Error;
    }
    render();
}

void UIPortalPage::stop_ap()
{
    if (state_ == State::Starting || state_ == State::Stopping) return;
    app_support::stop(job_pid_);
    unlink(log_path_.c_str());
    state_ = State::Stopping;
    last_poll_ = lv_tick_get();
    job_pid_ = app_support::spawn("( sudo -n sh '" + dir_ + "/stop.sh' ) >'" + log_path_ + "' 2>&1; echo \"#rc $?\" >>'" +
                                  log_path_ + "'");
    render();
}

void UIPortalPage::poll_job()
{
    if (job_pid_ <= 0) return;
    const std::string log = app_support::read_file(log_path_);
    const int rc = portal::parse_rc(log);
    if (rc < 0) return; // still running
    app_support::stop(job_pid_);
    const bool starting = state_ == State::Starting;
    if (rc == 0) {
        state_ = starting ? State::On : State::Off;
        error_.clear();
    } else {
        error_ = portal::last_line(log);
        if (error_.empty()) error_ = (starting ? "start failed" : "stop failed");
        // A failed stop still leaves the AP down from the page's view.
        state_ = starting ? State::Error : State::Off;
    }
    render();
}

void UIPortalPage::refresh_clients()
{
    clients_ = state_ == State::On ? portal::count_leases(app_support::read_file(dir_ + "/leases")) : 0;
}

void UIPortalPage::render()
{
    if (view_ == View::Naming) {
        if (status_) cp0::ui::set_text(status_, "name", cp0::theme::kAccent);
        if (big_) cp0::ui::set_text(big_, (name_buf_ + "_").c_str(), cp0::theme::kText);
        if (sub_) cp0::ui::set_text(sub_, "name the access point", cp0::theme::kIdle);
        if (info_) {
            const std::string info = "ch " + std::to_string(channel_) + "   open";
            cp0::ui::set_text(info_, info.c_str(), cp0::theme::kDim);
        }
        cp0::ui::set_footer(hint_, {{"ent", "save"}, {"esc", "home"}});
        return;
    }

    const char *state_text = "off";
    uint32_t color = cp0::theme::kIdle;
    switch (state_) {
    case State::Off: state_text = "off"; break;
    case State::Starting: state_text = "starting"; color = cp0::theme::kWarn; break;
    case State::On: state_text = "on"; color = cp0::theme::kAccent; break;
    case State::Stopping: state_text = "stopping"; color = cp0::theme::kWarn; break;
    case State::Error: state_text = "error"; color = cp0::theme::kDanger; break;
    }
    if (status_) cp0::ui::set_text(status_, state_text, color);

    if (big_) {
        const std::string head = std::string(portal::kInterface) + "  " + state_text;
        cp0::ui::set_text(big_, head.c_str(), color);
    }
    if (sub_) {
        std::string line;
        switch (state_) {
        case State::On: line = std::to_string(clients_) + (clients_ == 1 ? " client" : " clients"); break;
        case State::Starting: line = "bringing the ap up"; break;
        case State::Stopping: line = "tearing the ap down"; break;
        case State::Error: line = error_.empty() ? "failed" : error_; break;
        case State::Off: line = tools_ok_ ? "press enter to start" : "install hostapd and dnsmasq"; break;
        }
        cp0::ui::set_text(sub_, line.c_str(), state_ == State::Error ? cp0::theme::kDanger : cp0::theme::kIdle);
    }
    if (info_) {
        const std::string info = "ssid " + ssid_ + "   ch " + std::to_string(channel_) + "   " + portal::kGatewayIp +
                                 "   open";
        cp0::ui::set_text(info_, info.c_str(), cp0::theme::kDim);
    }

    const char *action = state_ == State::On ? "stop" : "start";
    if (state_ == State::On || state_ == State::Starting || state_ == State::Stopping)
        cp0::ui::set_footer(hint_, {{"ent", action}, {"esc", "home"}});
    else
        cp0::ui::set_footer(hint_, {{"ent", action}, {"e", "rename"}, {"esc", "home"}});
}

void UIPortalPage::on_timer()
{
    poll_job();
    if (state_ == State::On) {
        const uint32_t tick = lv_tick_get();
        if (tick - last_poll_ >= 2000) {
            last_poll_ = tick;
            const int before = clients_;
            // The AP can be killed from outside; reflect that.
            if (!ap_running()) {
                state_ = State::Off;
                refresh_clients();
                render();
                return;
            }
            refresh_clients();
            if (clients_ != before) render();
        }
    }
}

void UIPortalPage::handle_naming_key(uint32_t key, const key_item *item)
{
    switch (key) {
    case KEY_ENTER:
        commit_name(true);
        break;
    case KEY_ESC:
        commit_name(false);
        if (navigate_home) navigate_home();
        break;
    case KEY_BACKSPACE:
        if (launcher_ui::text_input::erase_last_codepoint(name_buf_)) render();
        break;
    default:
        if (launcher_ui::text_input::append_limited(name_buf_, launcher_ui::text_input::key_text(key, item), 32))
            render();
        break;
    }
}

void UIPortalPage::handle_control_key(uint32_t key)
{
    if (key == KEY_ESC) {
        if (navigate_home) navigate_home();
        return;
    }
    if (key == KEY_E && state_ == State::Off) {
        enter_name_edit();
        render();
        return;
    }
    if (key == KEY_ENTER) {
        if (state_ == State::On)
            stop_ap();
        else if (state_ == State::Off || state_ == State::Error)
            start_ap();
    }
}

void UIPortalPage::timer_cb(lv_timer_t *timer) noexcept
{
    auto *page = static_cast<UIPortalPage *>(lv_timer_get_user_data(timer));
    if (!page) return;
    try {
        page->on_timer();
    } catch (...) {
    }
}

void UIPortalPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UIPortalPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item) return;
    const uint32_t key = item->semantic_key ? item->semantic_key : item->key_code;
    const bool repeat = item->key_state == KBD_KEY_REPEATED;
    try {
        if (page->view_ == View::Naming) {
            // Repeats drive backspace only, so held printable keys do not run away.
            if (!repeat || key == KEY_BACKSPACE) page->handle_naming_key(key, item);
        } else if (!repeat) {
            page->handle_control_key(key);
        }
    } catch (...) {
    }
}

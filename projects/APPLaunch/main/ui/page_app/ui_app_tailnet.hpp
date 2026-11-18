/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "../model/tailnet_model.hpp"
#include "cp0_theme.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

class UISTPage;

// ============================================================
//  Tailnet  UITailnetPage
//  The machines on the Tailscale network, from `tailscale status --json`,
//  refreshed in the background. Shows "tailscale not installed" when the
//  CLI is missing.
//
//  Keys:
//    UP / DOWN   select a node
//    ENTER       ping the selected node (result in its row)
//    S           ssh to the selected node in the terminal page
//    E           use / stop using the selected node as exit node
//    ESC         home
// ============================================================
class UITailnetPage : public AppPage
{
public:
    UITailnetPage();
    ~UITailnetPage() override;

private:
    static constexpr int kRows = 8;

    // A shell command running in the background. Its stdout, stderr and exit
    // status land in <base>.out, <base>.err and <base>.rc; the .rc file is
    // written last, so its presence means the other two are complete.
    struct Job {
        int pid = -1;
        std::string base;
        uint32_t started = 0;
    };

    struct Row {
        lv_obj_t *bg = nullptr;
        lv_obj_t *dot = nullptr;
        lv_obj_t *text = nullptr;
        lv_obj_t *path = nullptr;
        cp0::ui::SegBar bar;
    };

    bool tool_ok_ = false;
    bool status_known_ = false;
    std::string status_error_;
    std::string backend_state_;
    std::vector<tailnet::Node> nodes_;
    std::map<std::string, tailnet::Ping> pings_;
    std::string selected_key_;
    int selected_ = 0;
    int top_ = 0;

    Job status_job_;
    Job ping_job_;
    Job exit_job_;
    std::string ping_ip_;
    bool exit_clearing_ = false;
    uint32_t last_status_ = 0;

    std::string message_;
    uint32_t message_color_ = cp0::theme::kAccent;
    uint32_t message_until_ = 0;

    std::shared_ptr<UISTPage> terminal_;

    lv_obj_t *status_ = nullptr;
    lv_obj_t *header_ = nullptr;
    lv_obj_t *count_ = nullptr;
    lv_obj_t *big_ = nullptr;
    lv_obj_t *sub_ = nullptr;
    std::array<Row, kRows> rows_{};
    lv_obj_t *hint_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    void create_ui();
    void show_message(const std::string &text, uint32_t color = cp0::theme::kAccent);

    static void start_job(Job &job, const std::string &command);
    static bool finish_job(Job &job, uint32_t timeout_ms, int &rc, std::string &out, std::string &err);
    static void cancel_job(Job &job);

    void start_status();
    void apply_status(int rc, const std::string &out, const std::string &err);
    void start_ping();
    void toggle_exit_node();
    void open_ssh();
    void close_ssh();
    static void close_ssh_cb(void *user_data) noexcept;

    const tailnet::Node *selected() const;
    const tailnet::Ping *ping_for(const tailnet::Node &node) const;
    static std::string key_of(const tailnet::Node &node);
    void move_selection(int delta);
    void clamp_view();

    void render();
    void on_timer();
    void handle_key(uint32_t key, bool repeat);
    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
};

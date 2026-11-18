/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_tailnet.hpp"
#include "ui_app_st.hpp"

#include "../app_support.hpp"
#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "input_keys.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace {

constexpr uint32_t kRefreshMs = 100;
constexpr uint32_t kStatusEveryMs = 10000;
constexpr uint32_t kStatusTimeoutMs = 8000;
constexpr uint32_t kPingTimeoutMs = 8000;
constexpr uint32_t kExitTimeoutMs = 10000;
constexpr uint32_t kMessageMs = 4000;
// The footer holds 44 columns of the 7 px font.
constexpr std::size_t kMessageColumns = 44;
constexpr int kStatusWidth = 175;

void set_visible(lv_obj_t *obj, bool on)
{
    if (!obj) return;
    if (on == !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    if (on) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

} // namespace

UITailnetPage::UITailnetPage() : AppPage()
{
    set_page_title("Tailnet");
    reclaim_top_bar();
    const std::string base = app_support::runtime_dir() + "/cp0-tailnet-" + std::to_string(getpid());
    status_job_.base = base + "-status";
    ping_job_.base = base + "-ping";
    exit_job_.base = base + "-exit";
    tool_ok_ = app_support::tool_present("tailscale");
    create_ui();
    if (tool_ok_) start_status();
    render();
    timer_ = lv_timer_create(timer_cb, kRefreshMs, this);
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

UITailnetPage::~UITailnetPage()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
    lv_async_call_cancel(close_ssh_cb, this);
    if (terminal_) terminal_->navigate_home = nullptr;
    terminal_.reset();
    cancel_job(status_job_);
    cancel_job(ping_job_);
    cancel_job(exit_job_);
}

void UITailnetPage::create_ui()
{
    if (!ui_APP_Container) return;
    cp0::ui::paint_background(ui_APP_Container);
    const cp0::ui::TitleRow title = cp0::ui::title_row(ui_APP_Container, "tailnet");
    status_ = title.status;
    if (status_) {
        // Wide enough for "tailscale not installed".
        lv_obj_set_pos(status_, cp0::ui::kStatusRight - kStatusWidth, cp0::theme::kTitleY);
        lv_obj_set_width(status_, kStatusWidth);
    }

    const cp0::ui::Panel panel = cp0::ui::panel(ui_APP_Container, 4, 19, 312, 132);
    if (panel.box) {
        header_ = cp0::ui::label(panel.box, 14, 3, cp0::theme::kDim);
        if (header_) lv_label_set_text(header_, "host       addr             ms path");
        count_ = cp0::ui::label(panel.box, 266, 3, cp0::theme::kDim);
        if (count_) {
            lv_obj_set_width(count_, 42);
            lv_obj_set_style_text_align(count_, LV_TEXT_ALIGN_RIGHT, 0);
        }
        cp0::ui::dotted_rule(panel.box, 4, 15, 304);

        for (int i = 0; i < kRows; ++i) {
            const int y = 19 + i * 14;
            Row &row = rows_[i];
            row.bg = cp0::ui::box(panel.box, 1, y - 1, 310, 12, cp0::theme::kSelect);
            row.dot = cp0::ui::box(panel.box, 5, y + 3, 4, 4, cp0::theme::kAccent);
            row.text = cp0::ui::label(panel.box, 14, y, cp0::theme::kText);
            row.path = cp0::ui::label(panel.box, 231, y, cp0::theme::kDim);
            row.bar = cp0::ui::segbar(panel.box, 282, y + 2, 24, 6);
        }

        big_ = cp0::ui::label(panel.box, 8, 30, cp0::theme::kText, true);
        sub_ = cp0::ui::label(panel.box, 8, 62, cp0::theme::kIdle);
        if (sub_) {
            lv_obj_set_width(sub_, 296);
            lv_label_set_long_mode(sub_, LV_LABEL_LONG_CLIP);
        }
    }
    hint_ = cp0::ui::footer(ui_APP_Container);
}

void UITailnetPage::show_message(const std::string &text, uint32_t color)
{
    message_ = tailnet::clip(text, kMessageColumns);
    // The footer label recolours text between '#' marks.
    std::replace(message_.begin(), message_.end(), '#', ' ');
    message_color_ = color;
    message_until_ = lv_tick_get() + kMessageMs;
    if (message_until_ == 0) message_until_ = 1;
}

void UITailnetPage::start_job(Job &job, const std::string &command)
{
    if (job.pid > 0 || command.empty()) return;
    unlink((job.base + ".rc").c_str());
    job.started = lv_tick_get();
    job.pid = app_support::spawn(command + " > '" + job.base + ".out' 2> '" + job.base + ".err'; echo $? > '" +
                                 job.base + ".rc'");
}

// True once the job has ended, by itself or by the timeout; fills rc, out and
// err and releases the job. Never waits for a running command.
bool UITailnetPage::finish_job(Job &job, uint32_t timeout_ms, int &rc, std::string &out, std::string &err)
{
    if (job.pid <= 0) return false;
    const std::string status = app_support::read_file(job.base + ".rc");
    const bool done = status.find('\n') != std::string::npos;
    if (!done && lv_tick_get() - job.started < timeout_ms) return false;
    // Stopping also reaps the shell when it has already exited.
    app_support::stop(job.pid);
    if (done) {
        rc = std::atoi(status.c_str());
        out = app_support::read_file(job.base + ".out");
        err = app_support::read_file(job.base + ".err");
    } else {
        rc = -1;
        out.clear();
        err = "timed out";
    }
    for (const char *suffix : {".out", ".err", ".rc"}) unlink((job.base + suffix).c_str());
    return true;
}

void UITailnetPage::cancel_job(Job &job)
{
    if (job.pid <= 0) return;
    app_support::stop(job.pid);
    for (const char *suffix : {".out", ".err", ".rc"}) unlink((job.base + suffix).c_str());
}

void UITailnetPage::start_status()
{
    if (!tool_ok_ || status_job_.pid > 0) return;
    last_status_ = lv_tick_get();
    start_job(status_job_, tailnet::status_cmd());
}

std::string UITailnetPage::key_of(const tailnet::Node &node)
{
    return node.ip.empty() ? node.host : node.ip;
}

void UITailnetPage::apply_status(int rc, const std::string &out, const std::string &err)
{
    status_known_ = true;
    tailnet::Status status;
    std::string parse_error;
    if (tailnet::parse_status(out, status, parse_error)) {
        status_error_.clear();
        backend_state_ = status.backend_state;
        nodes_ = std::move(status.nodes);
        tailnet::sort_nodes(nodes_);
    } else {
        // No document: the CLI failed (for example the daemon is not running).
        status_error_ = tailnet::first_line(err, kMessageColumns);
        if (status_error_.empty()) status_error_ = rc == 0 ? "unreadable status" : "tailscale status failed";
        backend_state_.clear();
        nodes_.clear();
    }
    selected_ = 0;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (key_of(nodes_[i]) == selected_key_) {
            selected_ = static_cast<int>(i);
            break;
        }
    }
    clamp_view();
}

const tailnet::Node *UITailnetPage::selected() const
{
    if (selected_ < 0 || selected_ >= static_cast<int>(nodes_.size())) return nullptr;
    return &nodes_[static_cast<std::size_t>(selected_)];
}

const tailnet::Ping *UITailnetPage::ping_for(const tailnet::Node &node) const
{
    if (node.ip.empty()) return nullptr;
    const auto found = pings_.find(node.ip);
    return found == pings_.end() ? nullptr : &found->second;
}

void UITailnetPage::clamp_view()
{
    const int count = static_cast<int>(nodes_.size());
    selected_ = std::clamp(selected_, 0, std::max(0, count - 1));
    if (selected_ < top_) top_ = selected_;
    if (selected_ >= top_ + kRows) top_ = selected_ - kRows + 1;
    top_ = std::clamp(top_, 0, std::max(0, count - kRows));
    if (const tailnet::Node *node = selected()) selected_key_ = key_of(*node);
}

void UITailnetPage::move_selection(int delta)
{
    if (nodes_.empty()) return;
    selected_ += delta;
    clamp_view();
    render();
}

void UITailnetPage::start_ping()
{
    const tailnet::Node *node = selected();
    if (!node || ping_job_.pid > 0) return;
    if (node->self) {
        show_message("this is the device itself", cp0::theme::kIdle);
    } else if (!node->online) {
        show_message(tailnet::clip(node->host, 20) + " is offline", cp0::theme::kIdle);
    } else if (tailnet::ping_cmd(node->ip).empty()) {
        show_message("no ipv4 address", cp0::theme::kWarn);
    } else {
        ping_ip_ = node->ip;
        pings_.erase(ping_ip_);
        start_job(ping_job_, tailnet::ping_cmd(ping_ip_));
        if (ping_job_.pid <= 0) {
            ping_ip_.clear();
            show_message("could not start tailscale ping", cp0::theme::kDanger);
        }
    }
    render();
}

void UITailnetPage::toggle_exit_node()
{
    const tailnet::Node *node = selected();
    if (!node || exit_job_.pid > 0) return;
    if (node->exit_node) {
        exit_clearing_ = true;
        start_job(exit_job_, tailnet::exit_node_cmd(""));
    } else if (!node->exit_option || node->self) {
        show_message(tailnet::clip(node->host, 20) + " does not offer an exit node", cp0::theme::kIdle);
        render();
        return;
    } else if (!tailnet::is_ipv4(node->ip)) {
        show_message("no ipv4 address", cp0::theme::kWarn);
        render();
        return;
    } else {
        exit_clearing_ = false;
        start_job(exit_job_, tailnet::exit_node_cmd(node->ip));
    }
    if (exit_job_.pid > 0)
        show_message(exit_clearing_ ? "clearing exit node..." : "setting exit node...", cp0::theme::kIdle);
    else
        show_message("could not start tailscale set", cp0::theme::kDanger);
    render();
}

void UITailnetPage::open_ssh()
{
    const tailnet::Node *node = selected();
    if (!node || terminal_) return;
    if (!tailnet::is_ipv4(node->ip)) {
        show_message("no ipv4 address", cp0::theme::kWarn);
        render();
        return;
    }
    if (!app_support::tool_present("ssh")) {
        show_message("ssh is not installed", cp0::theme::kDanger);
        render();
        return;
    }
    auto terminal = std::make_shared<UISTPage>();
    if (!terminal->screen()) {
        show_message("could not open a terminal", cp0::theme::kDanger);
        render();
        return;
    }
    terminal_ = terminal;
    terminal_->terminal_sysplause = false;
    // The terminal calls this from its own callbacks, so it is torn down later.
    terminal_->navigate_home = [this] { lv_async_call(close_ssh_cb, this); };
    cp0_lvgl_start_app_page(*terminal_);
    terminal_->exec("ssh", {node->ip});
}

void UITailnetPage::close_ssh_cb(void *user_data) noexcept
{
    auto *page = static_cast<UITailnetPage *>(user_data);
    if (!page) return;
    try {
        page->close_ssh();
    } catch (...) {
    }
}

void UITailnetPage::close_ssh()
{
    cp0_lvgl_start_app_page(*this);
    terminal_.reset();
    start_status();
    render();
}

void UITailnetPage::render()
{
    if (status_) {
        std::string text;
        uint32_t color = cp0::theme::kIdle;
        if (!tool_ok_) {
            text = "tailscale not installed";
            color = cp0::theme::kDanger;
        } else if (!status_known_) {
            text = "checking";
        } else if (!status_error_.empty()) {
            text = "no status";
            color = cp0::theme::kWarn;
        } else {
            text = tailnet::clip(tailnet::state_label(backend_state_), 24);
            if (backend_state_ == "Running") color = cp0::theme::kAccent;
        }
        cp0::ui::set_text(status_, text.c_str(), color);
    }

    const int count = static_cast<int>(nodes_.size());
    const bool empty = count == 0;
    set_visible(header_, !empty);
    set_visible(count_, count > kRows);
    if (count_ && count > kRows)
        cp0::ui::set_text(count_, (std::to_string(selected_ + 1) + "/" + std::to_string(count)).c_str(),
                          cp0::theme::kDim);

    set_visible(big_, empty);
    set_visible(sub_, empty);
    if (empty) {
        const char *sub = "reading tailscale status...";
        if (!tool_ok_) sub = "install tailscale to see your machines here";
        else if (!status_error_.empty()) sub = status_error_.c_str();
        else if (status_known_ && backend_state_ != "Running") sub = "tailscale is not connected";
        else if (status_known_) sub = "no machines in the status";
        cp0::ui::set_text(big_, "no tailnet", cp0::theme::kText);
        cp0::ui::set_text(sub_, sub, cp0::theme::kIdle);
    }

    for (int i = 0; i < kRows; ++i) {
        Row &row = rows_[i];
        const int index = top_ + i;
        const bool used = index < count;
        const bool on = used && index == selected_;
        set_visible(row.bg, on);
        set_visible(row.dot, used);
        set_visible(row.text, used);
        set_visible(row.path, used);
        if (!used) {
            set_visible(row.bar.track, false);
            continue;
        }
        const tailnet::Node &node = nodes_[static_cast<std::size_t>(index)];
        const tailnet::Ping *ping = ping_for(node);
        const tailnet::Path path = tailnet::path_of(node, ping);
        const bool offline = path == tailnet::Path::Offline;
        const bool relay = path == tailnet::Path::Relay;

        std::string text = tailnet::row_text(node, ping);
        if (ping_job_.pid > 0 && !node.ip.empty() && node.ip == ping_ip_ && text.size() >= 3)
            text.replace(text.size() - 3, 3, "...");
        cp0::ui::set_bg_color(row.dot, offline ? cp0::theme::kBracket : cp0::theme::kAccent);
        cp0::ui::set_text(row.text, text.c_str(),
                          offline ? cp0::theme::kIdle : on ? cp0::theme::kAccent : cp0::theme::kText);
        cp0::ui::set_text(row.path, tailnet::path_label(path),
                          offline ? cp0::theme::kBracket : relay ? cp0::theme::kWarn : cp0::theme::kDim);
        const bool bar = !offline && ping && ping->ok;
        set_visible(row.bar.track, bar);
        if (bar)
            cp0::ui::set_segbar(row.bar, tailnet::latency_fraction(ping->ms),
                                ping->relay ? cp0::theme::kWarn : cp0::theme::kAccent);
    }

    if (message_until_ != 0) {
        cp0::ui::set_footer_message(hint_, message_.c_str(), message_color_);
    } else if (empty) {
        cp0::ui::set_footer(hint_, {{"esc", "home"}});
    } else {
        const tailnet::Node *node = selected();
        std::vector<cp0::ui::Chip> chips = {{"ent", "ping"}, {"s", "ssh"}};
        if (node && node->exit_node) chips.push_back({"e", "stop exit node"});
        else if (node && node->exit_option && !node->self) chips.push_back({"e", "exit node"});
        chips.push_back({"up/dn", "select"});
        cp0::ui::set_footer(hint_, chips);
    }
}

void UITailnetPage::timer_cb(lv_timer_t *timer) noexcept
{
    auto *page = static_cast<UITailnetPage *>(lv_timer_get_user_data(timer));
    if (!page) return;
    try {
        page->on_timer();
    } catch (...) {
    }
}

void UITailnetPage::on_timer()
{
    // The ssh terminal owns the screen; results are picked up on return.
    if (terminal_) return;
    bool changed = false;
    int rc = 0;
    std::string out;
    std::string err;

    if (finish_job(status_job_, kStatusTimeoutMs, rc, out, err)) {
        apply_status(rc, out, err);
        changed = true;
    } else if (tool_ok_ && status_job_.pid <= 0 && lv_tick_get() - last_status_ >= kStatusEveryMs) {
        start_status();
        if (status_job_.pid <= 0 && !status_known_) {
            status_known_ = true;
            status_error_ = "could not start tailscale";
            changed = true;
        }
    }

    if (finish_job(ping_job_, kPingTimeoutMs, rc, out, err)) {
        tailnet::Ping ping = tailnet::parse_ping(out);
        if (!ping.ok) {
            // The reason is on stderr when the command failed outright.
            const std::string reason = tailnet::first_line(err, kMessageColumns);
            if (!reason.empty()) ping.error = reason;
            show_message("ping: " + ping.error, cp0::theme::kWarn);
        }
        if (!ping_ip_.empty()) pings_[ping_ip_] = ping;
        ping_ip_.clear();
        changed = true;
    }

    if (finish_job(exit_job_, kExitTimeoutMs, rc, out, err)) {
        if (rc == 0) {
            show_message(exit_clearing_ ? "exit node cleared" : "exit node set");
            start_status();
        } else {
            std::string reason = tailnet::first_line(err, kMessageColumns);
            if (reason.empty()) reason = tailnet::first_line(out, kMessageColumns);
            if (reason.empty()) reason = "tailscale set failed";
            show_message(reason, cp0::theme::kDanger);
        }
        changed = true;
    }

    if (message_until_ != 0 && static_cast<int32_t>(lv_tick_get() - message_until_) >= 0) {
        message_until_ = 0;
        changed = true;
    }
    if (changed) render();
}

void UITailnetPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UITailnetPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item) return;
    const uint32_t key = item->semantic_key ? item->semantic_key : item->key_code;
    try {
        page->handle_key(key, item->key_state == KBD_KEY_REPEATED);
    } catch (...) {
    }
}

void UITailnetPage::handle_key(uint32_t key, bool repeat)
{
    if (terminal_) return;
    if (repeat && key != KEY_UP && key != KEY_DOWN) return;
    if (key == KEY_ESC) {
        if (navigate_home) navigate_home();
        return;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        move_selection(key == KEY_DOWN ? 1 : -1);
        return;
    }
    if (!tool_ok_) return;
    if (key == KEY_ENTER || key == KEY_KPENTER) {
        start_ping();
    } else if (key == KEY_S) {
        open_ssh();
    } else if (key == KEY_E) {
        toggle_exit_node();
    }
}

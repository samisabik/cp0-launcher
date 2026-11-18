/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_air.hpp"

#include "../app_support.hpp"
#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "input_keys.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace {

constexpr uint32_t kRefreshMs = 100;
constexpr uint32_t kScanEveryMs = 10000;
constexpr uint32_t kScanTimeoutMs = 20000;
constexpr uint32_t kBleStatusEveryMs = 10000;
constexpr uint32_t kBleListEveryMs = 2000;
// How long to wait for the first Bluetooth status before reporting none.
constexpr uint32_t kBleAnswerMs = 2000;
constexpr uint32_t kMessageMs = 2500;
constexpr std::size_t kMessageColumns = 44;
constexpr int kStatusWidth = 175;

// Both tab containers sit below the title row; children use screen
// coordinates minus this offset.
constexpr int kTop = cp0::theme::kBodyTop;
constexpr int kMapX = 5;
constexpr int kMapY = 20 - kTop;
constexpr int kBaseY = 72;
constexpr float kMaxPeak = 58.0f;
// 2.4 GHz channel n is centred on canvas column kChan0 + n * kChanPx.
constexpr int kChan0 = 21;
constexpr int kChanPx = 19;
constexpr int kBarSlot = 23;
constexpr int kBarHalf = 7;
constexpr int kAxisW = 21;
constexpr int kNameColumns = 12;
constexpr int kGlyphW = 7;

constexpr const char *kViewConfigKey = "air_view";
constexpr const char *kAdapterConfigKey = "air_iface";

void set_visible(lv_obj_t *obj, bool on)
{
    if (!obj) return;
    if (on == !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    if (on) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

void set_pos(lv_obj_t *obj, int x, int y)
{
    if (!obj) return;
    if (lv_obj_get_style_x(obj, LV_PART_MAIN) != x || lv_obj_get_style_y(obj, LV_PART_MAIN) != y)
        lv_obj_set_pos(obj, x, y);
}

lv_obj_t *container(lv_obj_t *parent)
{
    lv_obj_t *obj = cp0::ui::box(parent, 0, kTop, cp0::theme::kScreenW, cp0::theme::kBodyH, cp0::theme::kBg);
    if (obj) lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    return obj;
}

std::string first_line(const std::string &text)
{
    std::size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    std::size_t end = text.find_first_of("\r\n", start);
    if (end == std::string::npos) end = text.size();
    return cp0::ui::lower(air::fit(text.substr(start, end - start), kMessageColumns, false));
}

int column_count(const std::string &text)
{
    int count = 0;
    for (const char c : text)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++count;
    return count;
}

} // namespace

UIAirPage::UIAirPage() : AppPage()
{
    set_page_title("Air");
    reclaim_top_bar();
    int stored = 0;
    cp0_signal_config_api({"GetInt", kViewConfigKey, "0"}, [&](int code, std::string data) {
        if (code == 0) stored = std::atoi(data.c_str());
    });
    band_ = stored / 10 == 1 ? air::Band::G5 : air::Band::G24;
    sort_ = stored % 10 == 1 ? air::Sort::Channel : stored % 10 == 2 ? air::Sort::Name : air::Sort::Signal;

    scan_job_.base = app_support::runtime_dir() + "/cp0-air-" + std::to_string(getpid()) + "-scan";
    ble_replies_ = std::make_shared<BleReplies>();
    pixels_.assign(static_cast<std::size_t>(kMapW) * kMapH, app_support::rgb565(cp0::theme::kPanel));
    nmcli_ok_ = app_support::tool_present("nmcli");
    cp0_signal_config_api({"GetStr", kAdapterConfigKey, ""}, [&](int code, std::string data) {
        if (code == 0) iface_ = data;
    });
    // A stored adapter that is unplugged falls back to the first one.
    const std::vector<std::string> adapters = app_support::wifi_interfaces();
    if (std::find(adapters.begin(), adapters.end(), iface_) == adapters.end())
        iface_ = adapters.empty() ? std::string() : adapters.front();
    create_ui();
    if (nmcli_ok_) start_scan();
    draw_map();
    render();
    timer_ = lv_timer_create(timer_cb, kRefreshMs, this);
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

UIAirPage::~UIAirPage()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
    leave_ble();
    cancel_job(scan_job_);
}

void UIAirPage::create_ui()
{
    if (!ui_APP_Container) return;
    cp0::ui::paint_background(ui_APP_Container);
    const cp0::ui::TitleRow title = cp0::ui::title_row(ui_APP_Container, "air");
    status_ = title.status;
    if (status_) {
        // Wide enough for "2.4 ghz  12 aps  best 11".
        lv_obj_set_pos(status_, cp0::ui::kStatusRight - kStatusWidth, cp0::theme::kTitleY);
        lv_obj_set_width(status_, kStatusWidth);
    }

    wifi_view_ = container(ui_APP_Container);
    if (wifi_view_) {
        cp0::ui::panel(wifi_view_, 4, 19 - kTop, 312, 86);
        canvas_ = lv_canvas_create(wifi_view_);
        if (canvas_) {
            lv_canvas_set_buffer(canvas_, pixels_.data(), kMapW, kMapH, LV_COLOR_FORMAT_RGB565);
            lv_obj_set_pos(canvas_, kMapX, kMapY);
        }
        for (lv_obj_t *&label : axis_) {
            label = cp0::ui::label(wifi_view_, 0, kMapY + kBaseY + 1, cp0::theme::kBracket);
            if (!label) continue;
            lv_obj_set_width(label, kAxisW);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        }
        for (lv_obj_t *&label : names_) label = cp0::ui::label(wifi_view_, 0, 0, cp0::theme::kIdle);
        wifi_big_ = cp0::ui::label(wifi_view_, 12, 30 - kTop, cp0::theme::kText, true);
        wifi_sub_ = cp0::ui::label(wifi_view_, 12, 60 - kTop, cp0::theme::kIdle);
        if (wifi_sub_) {
            lv_obj_set_width(wifi_sub_, 296);
            lv_label_set_long_mode(wifi_sub_, LV_LABEL_LONG_CLIP);
        }

        const cp0::ui::Panel list = cp0::ui::panel(wifi_view_, 4, 109 - kTop, 312, 42);
        for (int i = 0; list.box && i < kWifiRows; ++i) {
            const int y = 3 + i * 13;
            ListRow &row = wifi_rows_[i];
            row.bg = cp0::ui::box(list.box, 1, y - 1, 310, 12, cp0::theme::kSelect);
            row.text = cp0::ui::label(list.box, 5, y, cp0::theme::kText);
            row.bar = cp0::ui::segbar(list.box, 242, y + 2, 62, 6);
        }
    }

    ble_view_ = container(ui_APP_Container);
    if (ble_view_) {
        const cp0::ui::Panel panel = cp0::ui::panel(ble_view_, 4, 19 - kTop, 312, 132);
        if (panel.box) {
            ble_header_ = cp0::ui::label(panel.box, 5, 3, cp0::theme::kDim);
            if (ble_header_)
                lv_label_set_text(ble_header_, (air::fit("device", 23) + air::fit("state", 8) + "dbm").c_str());
            cp0::ui::dotted_rule(panel.box, 4, 15, 304);
            for (int i = 0; i < kBleRows; ++i) {
                const int y = 19 + i * 14;
                ListRow &row = ble_rows_[i];
                row.bg = cp0::ui::box(panel.box, 1, y - 1, 310, 12, cp0::theme::kSelect);
                row.text = cp0::ui::label(panel.box, 5, y, cp0::theme::kText);
                row.bar = cp0::ui::segbar(panel.box, 250, y + 2, 54, 6);
            }
            ble_big_ = cp0::ui::label(panel.box, 8, 30, cp0::theme::kText, true);
            ble_sub_ = cp0::ui::label(panel.box, 8, 62, cp0::theme::kIdle);
            if (ble_sub_) {
                lv_obj_set_width(ble_sub_, 296);
                lv_label_set_long_mode(ble_sub_, LV_LABEL_LONG_CLIP);
            }
        }
    }
    hint_ = cp0::ui::footer(ui_APP_Container);
}

void UIAirPage::show_message(const std::string &text)
{
    message_ = air::fit(text, kMessageColumns, false);
    // The footer label recolours text between '#' marks.
    std::replace(message_.begin(), message_.end(), '#', ' ');
    message_until_ = lv_tick_get() + kMessageMs;
    if (message_until_ == 0) message_until_ = 1;
}

void UIAirPage::save_view_config()
{
    const int sort = sort_ == air::Sort::Channel ? 1 : sort_ == air::Sort::Name ? 2 : 0;
    const int value = (band_ == air::Band::G5 ? 10 : 0) + sort;
    cp0_signal_config_api({"SetInt", kViewConfigKey, std::to_string(value)}, nullptr);
    cp0_signal_config_api({"Save"}, nullptr);
}

void UIAirPage::start_job(Job &job, const std::string &command)
{
    if (job.pid > 0 || command.empty()) return;
    unlink((job.base + ".rc").c_str());
    job.started = lv_tick_get();
    job.pid = app_support::spawn(command + " > '" + job.base + ".out' 2> '" + job.base + ".err'; echo $? > '" +
                                 job.base + ".rc'");
}

// True once the job has ended, by itself or by the timeout; fills rc, out and
// err and releases the job. Never waits for a running command.
bool UIAirPage::finish_job(Job &job, uint32_t timeout_ms, int &rc, std::string &out, std::string &err)
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
        err = "scan timed out";
    }
    for (const char *suffix : {".out", ".err", ".rc"}) unlink((job.base + suffix).c_str());
    return true;
}

void UIAirPage::cancel_job(Job &job)
{
    if (job.pid <= 0) return;
    app_support::stop(job.pid);
    for (const char *suffix : {".out", ".err", ".rc"}) unlink((job.base + suffix).c_str());
}

void UIAirPage::start_scan()
{
    if (!nmcli_ok_ || scan_job_.pid > 0) return;
    last_scan_ = lv_tick_get();
    start_job(scan_job_, air::scan_cmd(iface_));
}

void UIAirPage::next_adapter()
{
    const std::vector<std::string> adapters = app_support::wifi_interfaces();
    const std::string next = air::next_interface(adapters, iface_);
    if (next == iface_) {
        show_message(adapters.empty() ? "no wi-fi adapter" : "only " + iface_);
        return;
    }
    iface_ = next;
    cp0_signal_config_api({"SetStr", kAdapterConfigKey, iface_}, nullptr);
    cp0_signal_config_api({"Save"}, nullptr);
    cancel_job(scan_job_);
    wifi_known_ = false;
    all_.clear();
    rebuild_view();
    start_scan();
    show_message("scanning on " + iface_);
}

std::string UIAirPage::key_of(const air::Network &network)
{
    return network.ssid + "\n" + std::to_string(network.channel);
}

void UIAirPage::apply_scan(int rc, const std::string &out, const std::string &err)
{
    wifi_known_ = true;
    if (rc == 0) {
        wifi_error_.clear();
        all_ = air::parse_nmcli(out);
    } else {
        wifi_error_ = first_line(err);
        if (wifi_error_.empty()) wifi_error_ = first_line(out);
        if (wifi_error_.empty()) wifi_error_ = "nmcli failed";
        all_.clear();
    }
    rebuild_view();
}

void UIAirPage::rebuild_view()
{
    view_ = air::in_band(all_, band_);
    air::sort_networks(view_, sort_);
    wifi_selected_ = 0;
    for (std::size_t i = 0; i < view_.size(); ++i) {
        if (key_of(view_[i]) == wifi_key_) {
            wifi_selected_ = static_cast<int>(i);
            break;
        }
    }
    move_selection(0);
}

void UIAirPage::move_selection(int delta)
{
    if (tab_ == Tab::Wifi || delta == 0) {
        const int count = static_cast<int>(view_.size());
        if (tab_ == Tab::Wifi) wifi_selected_ += delta;
        wifi_selected_ = std::clamp(wifi_selected_, 0, std::max(0, count - 1));
        if (wifi_selected_ < wifi_top_) wifi_top_ = wifi_selected_;
        if (wifi_selected_ >= wifi_top_ + kWifiRows) wifi_top_ = wifi_selected_ - kWifiRows + 1;
        wifi_top_ = std::clamp(wifi_top_, 0, std::max(0, count - kWifiRows));
        if (count > 0) wifi_key_ = key_of(view_[static_cast<std::size_t>(wifi_selected_)]);
        draw_map();
    }
    if (tab_ == Tab::Ble || delta == 0) {
        const int count = static_cast<int>(ble_devices_.size());
        if (tab_ == Tab::Ble) ble_selected_ += delta;
        ble_selected_ = std::clamp(ble_selected_, 0, std::max(0, count - 1));
        if (ble_selected_ < ble_top_) ble_top_ = ble_selected_;
        if (ble_selected_ >= ble_top_ + kBleRows) ble_top_ = ble_selected_ - kBleRows + 1;
        ble_top_ = std::clamp(ble_top_, 0, std::max(0, count - kBleRows));
        if (count > 0) ble_key_ = ble_devices_[static_cast<std::size_t>(ble_selected_)].address;
    }
    render();
}

void UIAirPage::request_ble_status()
{
    last_ble_status_ = lv_tick_get();
    cp0_signal_bt_api({"BtStatus"}, [replies = ble_replies_](int code, std::string data) {
        std::lock_guard<std::mutex> lock(replies->mutex);
        replies->status_seen = true;
        replies->status_code = code;
        replies->status = std::move(data);
    });
}

void UIAirPage::request_ble_list()
{
    last_ble_list_ = lv_tick_get();
    cp0_signal_bt_api({"BtList", std::to_string(CP0_BT_DEVICE_MAX)},
                      [replies = ble_replies_](int code, std::string data) {
        std::lock_guard<std::mutex> lock(replies->mutex);
        replies->list_seen = true;
        replies->list = code >= 0 ? std::move(data) : std::string();
    });
}

void UIAirPage::enter_ble()
{
    ble_entered_ = lv_tick_get();
    ble_discovery_failed_ = false;
    request_ble_status();
}

// Discovery only runs while the tab is showing.
void UIAirPage::leave_ble()
{
    if (!ble_discovering_) return;
    ble_discovering_ = false;
    cp0_signal_bt_api({"BtDiscoveryStop"}, [](int, std::string) {});
}

// Moves replies that arrived since the last call into the page state.
bool UIAirPage::take_ble_replies()
{
    bool status_seen = false;
    bool list_seen = false;
    bool discovery_failed = false;
    int status_code = 0;
    std::string status;
    std::string list;
    {
        std::lock_guard<std::mutex> lock(ble_replies_->mutex);
        status_seen = ble_replies_->status_seen;
        list_seen = ble_replies_->list_seen;
        discovery_failed = ble_replies_->discovery_failed;
        status_code = ble_replies_->status_code;
        if (status_seen) status = std::move(ble_replies_->status);
        if (list_seen) list = std::move(ble_replies_->list);
        ble_replies_->status_seen = ble_replies_->list_seen = ble_replies_->discovery_failed = false;
    }
    if (status_seen) {
        bool powered = false;
        ble_known_ = true;
        ble_available_ = status_code == 0 && air::parse_bt_status(status, powered);
        ble_powered_ = ble_available_ && powered;
        if (!ble_powered_) {
            ble_devices_.clear();
            ble_discovering_ = false;
        }
    }
    if (discovery_failed) {
        ble_discovering_ = false;
        ble_discovery_failed_ = true;
    }
    if (list_seen && ble_powered_) {
        std::vector<air::BleDevice> devices = air::parse_bt_devices(list);
        // Remembered devices without a reading are not nearby.
        devices.erase(std::remove_if(devices.begin(), devices.end(),
                                     [](const air::BleDevice &d) { return d.rssi == 0 && !d.connected; }),
                      devices.end());
        air::sort_ble(devices);
        ble_devices_ = std::move(devices);
        ble_selected_ = 0;
        for (std::size_t i = 0; i < ble_devices_.size(); ++i) {
            if (ble_devices_[i].address == ble_key_) {
                ble_selected_ = static_cast<int>(i);
                break;
            }
        }
    }
    return status_seen || list_seen || discovery_failed;
}

void UIAirPage::set_tab(Tab tab)
{
    if (tab == tab_) return;
    if (tab_ == Tab::Ble) leave_ble();
    tab_ = tab;
    if (tab_ == Tab::Ble) enter_ble();
    else if (lv_tick_get() - last_scan_ >= kScanEveryMs) start_scan();
    move_selection(0);
}

void UIAirPage::plot(int x, int y, uint16_t color)
{
    if (x < 0 || x >= kMapW || y < 0 || y >= kMapH) return;
    pixels_[static_cast<std::size_t>(y) * kMapW + static_cast<std::size_t>(x)] = color;
}

void UIAirPage::line(int x0, int y0, int x1, int y1, uint16_t color)
{
    const int steps = std::max({std::abs(x1 - x0), std::abs(y1 - y0), 1});
    for (int i = 0; i <= steps; ++i)
        plot(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, color);
}

void UIAirPage::draw_map()
{
    if (!canvas_) return;
    const uint16_t accent = app_support::rgb565(cp0::theme::kAccent);
    const uint16_t dim = app_support::rgb565(cp0::theme::kDim);
    const uint16_t faint = app_support::rgb565(cp0::theme::kBracket);
    std::fill(pixels_.begin(), pixels_.end(), app_support::rgb565(cp0::theme::kPanel));
    for (int x = 4; x < kMapW - 4; x += 2) plot(x, kBaseY, app_support::rgb565(cp0::theme::kLine));

    const int count = static_cast<int>(view_.size());
    const air::Network *chosen = count > 0 ? &view_[static_cast<std::size_t>(wifi_selected_)] : nullptr;

    // Strongest first, for the curve colours and the name labels.
    std::vector<const air::Network *> by_signal;
    for (const air::Network &network : view_) by_signal.push_back(&network);
    std::stable_sort(by_signal.begin(), by_signal.end(),
                     [](const air::Network *a, const air::Network *b) { return a->signal > b->signal; });

    struct Peak {
        const air::Network *network;
        int x;
        int height;
    };
    std::vector<Peak> peaks;
    std::size_t axis_used = 0;

    if (band_ == air::Band::G24) {
        for (int ch = 1; ch <= kAxisLabels; ++ch) {
            lv_obj_t *label = axis_[static_cast<std::size_t>(ch - 1)];
            set_pos(label, kMapX + kChan0 + ch * kChanPx - kAxisW / 2, kMapY + kBaseY + 1);
            cp0::ui::set_text(label, std::to_string(ch).c_str(),
                              ch == 1 || ch == 6 || ch == 11 ? cp0::theme::kIdle : cp0::theme::kBracket);
        }
        axis_used = kAxisLabels;

        // Weakest first so stronger curves are drawn over them; the selection goes last.
        for (int pass = 0; pass < 2; ++pass) {
            for (int rank = count - 1; rank >= 0; --rank) {
                const air::Network *network = by_signal[static_cast<std::size_t>(rank)];
                if ((network == chosen) != (pass == 1)) continue;
                const uint16_t color = network == chosen ? accent : rank < kNameLabels ? dim : faint;
                const float peak = air::peak_height(network->signal, kMaxPeak);
                const int centre = kChan0 + network->channel * kChanPx;
                const int reach = static_cast<int>(air::kHalfWidthChannels * kChanPx);
                int last_x = 0;
                int last_y = 0;
                for (int d = -reach; d <= reach; ++d) {
                    const float at = static_cast<float>(network->channel) + static_cast<float>(d) / kChanPx;
                    const int y = kBaseY - static_cast<int>(std::lround(air::curve_height(network->channel, at, peak)));
                    if (d > -reach) line(last_x, last_y, centre + d, y, color);
                    last_x = centre + d;
                    last_y = y;
                }
            }
        }
        for (const air::Network *network : by_signal)
            peaks.push_back({network, kChan0 + network->channel * kChanPx,
                             static_cast<int>(std::lround(air::peak_height(network->signal, kMaxPeak)))});
    } else {
        const std::vector<air::ChannelBar> bars = air::channel_bars(view_, kAxisLabels);
        const int left = (kMapW - static_cast<int>(bars.size()) * kBarSlot) / 2;
        for (std::size_t i = 0; i < bars.size(); ++i) {
            const int centre = left + static_cast<int>(i) * kBarSlot + kBarSlot / 2;
            const int height = static_cast<int>(std::lround(air::peak_height(bars[i].signal, kMaxPeak)));
            const bool on = chosen && chosen->channel == bars[i].channel;
            for (int y = kBaseY - height; y < kBaseY; ++y)
                for (int x = centre - kBarHalf; x <= centre + kBarHalf; ++x)
                    plot(x, y, on ? accent : faint);
            set_pos(axis_[i], kMapX + centre - kAxisW / 2, kMapY + kBaseY + 1);
            cp0::ui::set_text(axis_[i], std::to_string(bars[i].channel).c_str(),
                              on ? cp0::theme::kIdle : cp0::theme::kBracket);
            // Label each bar with the strongest network on that channel.
            for (const air::Network *network : by_signal) {
                if (network->channel != bars[i].channel) continue;
                peaks.push_back({network, centre, height});
                break;
            }
        }
        axis_used = bars.size();
        std::stable_sort(peaks.begin(), peaks.end(),
                         [](const Peak &a, const Peak &b) { return a.network->signal > b.network->signal; });
    }
    for (std::size_t i = 0; i < axis_.size(); ++i) set_visible(axis_[i], i < axis_used);

    // Names over the strongest peaks, skipping one that would cover another.
    struct Placed {
        int x, y, w;
    };
    std::vector<Placed> placed;
    for (const Peak &peak : peaks) {
        if (placed.size() >= names_.size()) break;
        const std::string name = air::fit(air::display_name(*peak.network), kNameColumns, false);
        const int w = column_count(name) * kGlyphW;
        const int x = std::clamp(kMapX + peak.x - w / 2, kMapX + 2, kMapX + kMapW - 2 - w);
        const int y = std::max(kMapY + 1, kMapY + kBaseY - peak.height - 12);
        const bool clash = std::any_of(placed.begin(), placed.end(), [&](const Placed &p) {
            return x < p.x + p.w + 4 && p.x < x + w + 4 && std::abs(y - p.y) < 11;
        });
        if (clash) continue;
        lv_obj_t *label = names_[placed.size()];
        set_pos(label, x, y);
        cp0::ui::set_text(label, name.c_str(), peak.network == chosen ? cp0::theme::kText : cp0::theme::kIdle);
        placed.push_back({x, y, w});
    }
    for (std::size_t i = 0; i < names_.size(); ++i) set_visible(names_[i], i < placed.size());
    lv_obj_invalidate(canvas_);
}

void UIAirPage::render()
{
    const bool wifi = tab_ == Tab::Wifi;
    set_visible(wifi_view_, wifi);
    set_visible(ble_view_, !wifi);

    std::string status;
    uint32_t status_color = cp0::theme::kIdle;
    if (wifi) {
        const int count = static_cast<int>(view_.size());
        const char *big = "";
        std::string sub;
        if (!nmcli_ok_) {
            status = "nmcli not installed";
            status_color = cp0::theme::kDanger;
            big = "no scan";
            sub = "install network-manager to scan wi-fi";
        } else if (!wifi_known_) {
            status = "scanning";
            sub = "scanning...";
        } else if (!wifi_error_.empty()) {
            status = "scan failed";
            status_color = cp0::theme::kWarn;
            big = "no scan";
            sub = wifi_error_;
        } else {
            status = std::string(air::band_label(band_)) + "  " + std::to_string(count) + (count == 1 ? " ap" : " aps");
            if (count > 0) status_color = cp0::theme::kAccent;
            if (count > 0 && band_ == air::Band::G24)
                status += "  best " + std::to_string(air::quietest_channel(air::congestion(view_)));
            if (count == 0) sub = std::string("no networks on ") + air::band_label(band_);
        }
        cp0::ui::set_text(wifi_big_, big, cp0::theme::kText);
        cp0::ui::set_text(wifi_sub_, sub.c_str(), cp0::theme::kIdle);

        for (int i = 0; i < kWifiRows; ++i) {
            ListRow &row = wifi_rows_[i];
            const int index = wifi_top_ + i;
            const bool used = index < count;
            const bool on = used && index == wifi_selected_;
            set_visible(row.bg, on);
            set_visible(row.text, used);
            set_visible(row.bar.track, used);
            if (!used) continue;
            const air::Network &network = view_[static_cast<std::size_t>(index)];
            cp0::ui::set_text(row.text, air::row_text(network).c_str(), on ? cp0::theme::kAccent : cp0::theme::kText);
            cp0::ui::set_segbar(row.bar, static_cast<float>(network.signal) / 100.0f, cp0::theme::kAccent);
        }
    } else {
        const int count = static_cast<int>(ble_devices_.size());
        const char *big = "";
        const char *sub = "";
        if (!ble_known_) {
            const bool waited = lv_tick_get() - ble_entered_ >= kBleAnswerMs;
            status = waited ? "no bluetooth" : "ble  checking";
            if (waited) {
                big = "no bluetooth";
                sub = "the bluetooth service did not answer";
            }
        } else if (!ble_available_) {
            status = "no bluetooth";
            status_color = cp0::theme::kWarn;
            big = "no bluetooth";
            sub = "the bluetooth service is unavailable";
        } else if (!ble_powered_) {
            status = "bluetooth off";
            big = "bluetooth off";
            sub = "turn it on in settings to scan";
        } else {
            status = "ble  " + std::to_string(count) + " nearby";
            if (count > 0) status_color = cp0::theme::kAccent;
            if (count == 0) sub = ble_discovery_failed_ ? "could not start the scan" : "scanning...";
        }
        set_visible(ble_header_, count > 0);
        cp0::ui::set_text(ble_big_, big, cp0::theme::kText);
        cp0::ui::set_text(ble_sub_, sub, cp0::theme::kIdle);

        for (int i = 0; i < kBleRows; ++i) {
            ListRow &row = ble_rows_[i];
            const int index = ble_top_ + i;
            const bool used = index < count;
            const bool on = used && index == ble_selected_;
            set_visible(row.bg, on);
            set_visible(row.text, used);
            set_visible(row.bar.track, used);
            if (!used) continue;
            const air::BleDevice &device = ble_devices_[static_cast<std::size_t>(index)];
            cp0::ui::set_text(row.text, air::ble_row_text(device).c_str(),
                              on ? cp0::theme::kAccent : cp0::theme::kText);
            cp0::ui::set_segbar(row.bar, air::rssi_fraction(device.rssi), cp0::theme::kAccent);
        }
    }
    cp0::ui::set_text(status_, status.c_str(), status_color);

    if (message_until_ != 0)
        cp0::ui::set_footer_message(hint_, message_.c_str());
    else if (wifi)
        cp0::ui::set_footer(hint_, {{"tab", "ble"}, {"5", band_ == air::Band::G24 ? "5 ghz" : "2.4 ghz"},
                                    {"s", "sort"}, {"w", iface_.empty() ? "adapter" : iface_.c_str()},
                                    {"up/dn", "select"}});
    else
        cp0::ui::set_footer(hint_, {{"tab", "wi-fi"}, {"up/dn", "select"}});
}

void UIAirPage::timer_cb(lv_timer_t *timer) noexcept
{
    auto *page = static_cast<UIAirPage *>(lv_timer_get_user_data(timer));
    if (!page) return;
    try {
        page->on_timer();
    } catch (...) {
    }
}

void UIAirPage::on_timer()
{
    bool changed = false;
    int rc = 0;
    std::string out;
    std::string err;
    if (finish_job(scan_job_, kScanTimeoutMs, rc, out, err)) {
        // Redraws through rebuild_view.
        apply_scan(rc, out, err);
    } else if (tab_ == Tab::Wifi && nmcli_ok_ && scan_job_.pid <= 0 && lv_tick_get() - last_scan_ >= kScanEveryMs) {
        start_scan();
        if (scan_job_.pid <= 0 && !wifi_known_) {
            wifi_known_ = true;
            wifi_error_ = "could not start nmcli";
            changed = true;
        }
    }

    if (tab_ == Tab::Ble) {
        const uint32_t now = lv_tick_get();
        if (now - last_ble_status_ >= kBleStatusEveryMs) request_ble_status();
        if (ble_powered_ && !ble_discovering_ && !ble_discovery_failed_) {
            ble_discovering_ = true;
            cp0_signal_bt_api({"BtDiscoveryStart"}, [replies = ble_replies_](int code, std::string) {
                if (code == 0) return;
                std::lock_guard<std::mutex> lock(replies->mutex);
                replies->discovery_failed = true;
            });
            request_ble_list();
        }
        if (ble_powered_ && now - last_ble_list_ >= kBleListEveryMs) request_ble_list();
        if (take_ble_replies()) {
            move_selection(0);
        } else if (!ble_known_ && now - ble_entered_ >= kBleAnswerMs &&
                   now - ble_entered_ < kBleAnswerMs + 2 * kRefreshMs) {
            changed = true;
        }
    }

    if (message_until_ != 0 && static_cast<int32_t>(lv_tick_get() - message_until_) >= 0) {
        message_until_ = 0;
        changed = true;
    }
    if (changed) render();
}

void UIAirPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UIAirPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item) return;
    const uint32_t key = item->semantic_key ? item->semantic_key : item->key_code;
    try {
        page->handle_key(key, item->key_state == KBD_KEY_REPEATED);
    } catch (...) {
    }
}

void UIAirPage::handle_key(uint32_t key, bool repeat)
{
    if (repeat && key != KEY_UP && key != KEY_DOWN) return;
    if (key == KEY_ESC) {
        if (navigate_home) navigate_home();
        return;
    }
    if (key == KEY_TAB) {
        set_tab(tab_ == Tab::Wifi ? Tab::Ble : Tab::Wifi);
        return;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        move_selection(key == KEY_DOWN ? 1 : -1);
        return;
    }
    if (tab_ != Tab::Wifi) return;
    if (key == KEY_5) {
        band_ = band_ == air::Band::G24 ? air::Band::G5 : air::Band::G24;
        save_view_config();
        rebuild_view();
    } else if (key == KEY_S) {
        sort_ = air::next_sort(sort_);
        save_view_config();
        show_message(std::string("sort by ") + air::sort_label(sort_));
        rebuild_view();
    } else if (key == KEY_W) {
        next_adapter();
    }
}

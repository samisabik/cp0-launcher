/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "../model/gps_model.hpp"
#include "cp0_theme.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

// ============================================================
//  GPS  UIGpsPage
//  Position logger for a serial GPS receiver on the Grove port (M5Stack GPS
//  Unit v1.1). Opening the page powers the port, routes it to the UART and
//  asks the receiver for its fastest report rate; leaving the page puts the
//  rate back. A log is a CSV file in ~/gps with one row per position.
//
//  Keys:
//    ENTER / SPACE  start a log, or stop and save the running one
//    ESC            stop the log and go home
// ============================================================
class UIGpsPage : public AppPage
{
public:
    UIGpsPage();
    ~UIGpsPage() override;

private:
    static constexpr int kInfoRows = 3;

    int fd_ = -1;
    gps::Parser parser_;
    gps::Fix fix_;
    bool have_report_ = false;
    uint32_t last_data_ms_ = 0;
    uint32_t last_open_ms_ = 0;
    uint32_t last_rate_request_ms_ = 0;

    uint32_t window_start_ms_ = 0;
    int window_reports_ = 0;
    float rate_hz_ = 0;

    std::string dir_;
    std::FILE *log_ = nullptr;
    std::string log_file_;
    uint32_t log_started_ms_ = 0;
    uint32_t log_flushed_ms_ = 0;
    long log_rows_ = 0;
    std::string message_;

    lv_obj_t *status_ = nullptr;
    cp0::ui::Readout lat_;
    cp0::ui::Readout lon_;
    cp0::ui::Readout speed_;
    cp0::ui::Readout alt_;
    cp0::ui::Readout sats_;
    std::array<lv_obj_t *, kInfoRows> info_{};
    lv_obj_t *footer_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    void create_ui();
    void open_port();
    void close_port();
    void request_rate(int interval_ms);
    void read_port();
    void take(const gps::Fix &fix);
    bool receiving() const;
    void start_log();
    void stop_log(const char *reason);
    void render();
    void on_timer();
    void handle_key(uint32_t key);

    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
};

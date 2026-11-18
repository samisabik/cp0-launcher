/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "../model/pins_model.hpp"
#include "cp0_theme.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ============================================================
//  Pins  UIPinsPage
//  Two tabs for the expansion header.
//    I2C    scans a bus for devices with a one-byte read per address.
//           Nothing is written and nothing is probed before ENTER.
//    Logic  watches up to four GPIO lines as inputs with kernel edge
//           events and draws them as traces; two of them can be decoded
//           as I2C. Lines come from the config keys pins_lines and
//           pins_i2c.
//
//  Keys:
//    TAB            switch tab
//    I2C:   LEFT / RIGHT bus, ENTER scan
//    Logic: ENTER arm / stop, Z zoom, LEFT / RIGHT scroll when stopped
//    ESC            home
// ============================================================
class UIPinsPage : public AppPage
{
public:
    UIPinsPage();
    ~UIPinsPage() override;

private:
    enum class Tab { I2c, Logic };

    static constexpr int kCellW = 17;
    static constexpr int kCellH = 10;
    static constexpr int kGridW = kCellW * pins::kGridColumns;
    static constexpr int kGridH = kCellH * pins::kGridRows;
    static constexpr int kDivPx = 34;
    static constexpr int kDivs = 8;
    static constexpr int kTraceW = kDivPx * kDivs;
    static constexpr int kTraceH = 84;
    static constexpr int kTracePitch = 22;
    static constexpr int kTraceAmp = 10;

    Tab tab_ = Tab::I2c;
    std::string message_;
    uint32_t message_until_ = 0;

    // I2C scan. The worker fills shared_probes_ under scan_mutex_.
    std::vector<int> buses_;
    int bus_index_ = 0;
    pins::ProbeMap probes_{};
    bool scanned_ = false;
    std::string scan_error_;
    std::thread scan_thread_;
    std::atomic<bool> scan_stop_{false};
    std::atomic<bool> scan_running_{false};
    std::atomic<int> scan_address_{0};
    std::mutex scan_mutex_;
    pins::ProbeMap shared_probes_{};
    std::string shared_scan_error_;
    bool scan_was_running_ = false;

    // Logic capture. The worker owns the line descriptors and appends edges
    // under logic_mutex_.
    std::vector<pins::LineRef> lines_;
    int i2c_scl_ = -1;
    int i2c_sda_ = -1;
    std::string chip_help_;
    int64_t div_ns_ = 1000000;
    int scroll_divs_ = 0;
    bool armed_ = false;
    bool captured_ = false;
    int64_t stop_ns_ = 0;
    bool logic_dirty_ = true;
    uint32_t clock_hz_ = 0;
    std::thread logic_thread_;
    std::atomic<bool> logic_stop_{false};
    std::atomic<uint32_t> lost_{0};
    std::mutex logic_mutex_;
    std::array<std::vector<pins::Edge>, pins::kMaxLines> edges_;
    std::array<bool, pins::kMaxLines> initial_{};
    std::array<std::string, pins::kMaxLines> line_error_;

    std::vector<uint16_t> grid_pixels_;
    std::vector<uint16_t> trace_pixels_;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *i2c_view_ = nullptr;
    lv_obj_t *logic_view_ = nullptr;
    lv_obj_t *bus_label_ = nullptr;
    lv_obj_t *grid_canvas_ = nullptr;
    lv_obj_t *found_label_ = nullptr;
    lv_obj_t *scan_label_ = nullptr;
    lv_obj_t *trace_canvas_ = nullptr;
    std::array<lv_obj_t *, pins::kMaxLines> line_labels_{};
    lv_obj_t *decode_label_ = nullptr;
    lv_obj_t *error_label_ = nullptr;
    lv_obj_t *info_label_ = nullptr;
    lv_obj_t *help_head_ = nullptr;
    lv_obj_t *help_body_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    void create_ui();
    void create_i2c_view();
    void create_logic_view();
    void show_message(const std::string &text);

    void find_buses();
    void start_scan();
    void stop_scan();
    void scan_loop(int bus);
    void draw_grid();

    void load_lines();
    void describe_chips();
    void arm();
    void disarm();
    void logic_loop();
    void draw_traces();

    void set_tab(Tab tab);
    void render();
    void on_timer();
    void handle_key(uint32_t key, bool repeat);
    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
};

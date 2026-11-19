/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "../model/sensors_model.hpp"
#include "../spectrum_scanner.hpp"
#include "cp0_theme.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================
//  Sensors  UISensorsPage
//  Dashboard of the 433 / 868 MHz thermometers and weather stations that
//  rtl_433 decodes: latest temperature, humidity, battery state, a
//  temperature sparkline and the time since each was last heard.
//
//  Two radios can feed it. The CC1101 on the hat is the default: it receives
//  OOK only, and each packet it captures is decoded by `rtl_433 -r`. An
//  RTL-SDR dongle, tuned by rtl_433 itself, also hears FSK sensors.
//
//  Keys:
//    UP / DOWN      select sensor (scrolls)
//    LEFT / RIGHT   switch between 433.92 and 868.30 MHz
//    R              switch between the CC1101 and the RTL-SDR
//    S              append the table to ~/sensors/sensors.log
//    ESC            home
// ============================================================
class UISensorsPage : public AppPage
{
public:
    UISensorsPage();
    ~UISensorsPage() override;

private:
    static constexpr int kRows = 4;
    static constexpr int kSparkW = sensors::kHistorySize;
    static constexpr int kSparkH = 18;

    struct RowView {
        cp0::ui::Panel panel;
        lv_obj_t *temperature = nullptr;
        lv_obj_t *unit = nullptr;
        lv_obj_t *name = nullptr;
        lv_obj_t *humidity = nullptr;
        lv_obj_t *battery = nullptr;
        lv_obj_t *age = nullptr;
        lv_obj_t *spark = nullptr;
        std::vector<uint16_t> pixels;
    };

    bool tools_ok_ = false;
    bool can_detect_ = false;
    bool present_ = false;
    int detect_pid_ = -1;
    int proc_pid_ = -1;
    std::string detect_path_;
    std::string out_path_;
    long out_offset_ = 0;
    std::string pending_;
    uint32_t last_detect_ = 0;

    sensors::Source source_ = sensors::Source::Cc1101;
    SpectrumScanner scanner_;
    bool radio_ok_ = false;
    std::string radio_error_;
    uint32_t last_radio_try_ = 0;
    std::vector<std::string> packets_;
    std::string ook_path_;
    uint32_t last_render_ = 0;

    sensors::Table table_;
    int frequency_index_ = 0;
    int selected_ = 0;
    int scroll_ = 0;
    std::string message_;
    uint32_t message_until_ = 0;

    lv_obj_t *status_ = nullptr;
    std::array<RowView, kRows> rows_{};
    lv_obj_t *big_ = nullptr;
    lv_obj_t *sub_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    void create_ui();
    void show_message(const std::string &text);

    void start_detect();
    void poll_detect();
    void start_receiver();
    void stop_receiver();
    bool read_lines();
    void set_frequency(int index);
    void set_source(sensors::Source source);
    void start_source();
    void stop_source();
    void start_radio();
    void stop_radio();
    bool poll_radio();
    uint32_t frequency_hz() const;
    void save_log();

    void draw_sparkline(RowView &row, const sensors::Sensor &sensor, bool focused);
    void render();
    void on_timer();
    void handle_key(uint32_t key, bool repeat);
    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
};

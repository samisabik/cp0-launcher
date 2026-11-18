/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "../model/fox_model.hpp"
#include "../spectrum_scanner.hpp"
#include "cp0_theme.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ============================================================
//  Fox  UIFoxPage
//  Transmitter hunting with the CC1101 on one frequency: live RSSI, peak
//  hold, a level bar, a scrolling history and a warmer / colder trend.
//
//  Keys:
//    LEFT / RIGHT   previous / next frequency
//    H              reset the peak hold
//    Z              zero: show the level relative to now; again to clear
//    B              beep on / off (shown when pacat is installed)
//    ESC            home
// ============================================================
class UIFoxPage : public AppPage
{
public:
    UIFoxPage();
    ~UIFoxPage() override;

private:
    static constexpr int kReadsPerSecond = 25;
    static constexpr int kBarX = 10;
    static constexpr int kBarW = 300;
    static constexpr int kStripX = 10;
    static constexpr int kStripY = 96;
    static constexpr int kStripW = 300;
    static constexpr int kStripH = 50;

    enum class Radio { Opening, Ready, Failed };

    // Worker side. The worker owns the radio; the LVGL thread only sets the
    // frequency and collects readings.
    Cc1101 radio_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<int> radio_state_{static_cast<int>(Radio::Opening)};
    std::atomic<uint32_t> freq_hz_{433920000u};
    std::atomic<bool> beep_on_{false};
    std::atomic<int> beep_interval_ms_{0};
    std::mutex mutex_;
    std::vector<int> pending_;
    std::string radio_error_;

    std::size_t freq_index_ = 0;
    fox::Tracker tracker_{kReadsPerSecond};
    fox::History history_{static_cast<std::size_t>(kStripW), 2};
    bool beep_available_ = false;
    int audio_pid_ = -1;
    std::string audio_path_;
    Radio shown_state_ = Radio::Opening;

    std::vector<uint16_t> pixels_;
    lv_obj_t *status_ = nullptr;
    cp0::ui::Readout rssi_;
    cp0::ui::Readout peak_;
    lv_obj_t *trend_ = nullptr;
    cp0::ui::SegBar bar_;
    lv_obj_t *peak_mark_ = nullptr;
    lv_obj_t *zero_mark_ = nullptr;
    lv_obj_t *canvas_ = nullptr;
    lv_obj_t *span_ = nullptr;
    lv_obj_t *big_ = nullptr;
    lv_obj_t *sub_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    void create_ui();
    void worker_loop();
    void set_frequency(std::size_t index);
    void set_beep(bool on);
    void render();
    void draw_strip();
    void on_timer();
    void handle_key(uint32_t key, bool repeat);
    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
};

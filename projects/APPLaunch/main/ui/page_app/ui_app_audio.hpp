/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "../model/audio_model.hpp"
#include "cp0_theme.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================
//  Audio  UIAudioPage
//  Microphone spectrum analyser and frequency finder. arecord streams
//  32 kHz mono into a FIFO; each frame takes a 4096-point FFT, shows the
//  dominant frequency with its nearest note, and draws log-spaced bars
//  from 40 Hz to 15 kHz.
//
//  Keys:
//    SPACE          freeze / resume
//    UP / DOWN      raise / lower the detection gate
//    R              clear the peak hold
//    ESC            home
// ============================================================
class UIAudioPage : public AppPage
{
public:
    UIAudioPage();
    ~UIAudioPage() override;

private:
    static constexpr int kWidth = 310;
    static constexpr int kCanvasX = 5;
    static constexpr int kCanvasTop = 64;
    static constexpr int kCanvasH = 74;
    static constexpr int kTickY = 141;
    static constexpr int kBands = 62;
    static constexpr int kBandW = kWidth / kBands;

    int rec_pid_ = -1;
    int fifo_fd_ = -1;
    std::string fifo_path_;
    bool odd_byte_valid_ = false;
    uint8_t odd_byte_ = 0;
    std::vector<int16_t> samples_;

    bool frozen_ = false;
    bool mic_ok_ = false;
    int gate_db_ = -60;
    uint32_t last_data_ms_ = 0;
    audio::Peak peak_;
    float last_hz_ = 0.0f;
    float level_db_ = audio::kFloorDb;
    std::array<float, kBands> bars_{};
    std::array<float, kBands> hold_{};
    std::string message_;
    uint32_t message_until_ = 0;

    std::vector<uint16_t> pixels_;
    lv_obj_t *status_ = nullptr;
    cp0::ui::Readout freq_;
    cp0::ui::Readout note_;
    lv_obj_t *gate_label_ = nullptr;
    lv_obj_t *canvas_ = nullptr;
    lv_obj_t *footer_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    void create_ui();
    bool start_capture();
    void stop_capture();
    void read_capture();
    void analyse();
    void draw();
    void render();
    void reset_hold();
    void show_message(const std::string &text);
    void on_timer();
    void handle_key(uint32_t key);

    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
};

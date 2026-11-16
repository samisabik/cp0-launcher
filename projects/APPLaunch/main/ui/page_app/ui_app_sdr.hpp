/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "../model/sdr_model.hpp"
#include "cp0_theme.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// ============================================================
//  SDR  UISdrPage
//  RTL-SDR front end with three modes, driven by the rtl-sdr CLI tools.
//    Scope   wideband spectrum + waterfall over a 2 MHz span (rtl_power)
//    Listen  tune and demodulate to the speaker (rtl_fm | aplay)
//    Decode  named devices in range (rtl_433)
//  Shows "connect RTL-SDR" until a dongle is detected.
//
//  Keys:
//    TAB            cycle mode
//    LEFT / RIGHT   tune centre by the step
//    UP / DOWN      bigger / smaller tuning step
//    1..6           presets (fm air 433 pmr noaa ads-b)
//    G              cycle gain (auto / 20 / 30 / 40 dB)
//    Scope: S span, H clear peak hold
//    Listen: M demod mode, ENTER play, SPACE stop, O audio out on/off
//    Decode: ENTER start/stop, S save log
//    ESC            home
// ============================================================
class UISdrPage : public AppPage
{
public:
    UISdrPage();
    ~UISdrPage() override;

private:
    enum class Mode { Scope, Listen, Decode };

    static constexpr int kWidth = 310;
    static constexpr int kCanvasTop = 64;
    static constexpr int kCanvasH = 86;
    static constexpr int kTraceH = 40;

    Mode mode_ = Mode::Scope;
    bool present_ = false;
    bool tools_ok_ = true;
    uint32_t center_hz_ = 433920000u;
    uint32_t span_hz_ = 2000000u;
    int step_index_ = 3;
    int gain_db_ = -1;
    sdr::Demod demod_ = sdr::Demod::NFM;
    bool playing_ = false;
    bool audio_out_ = true;
    bool decoding_ = false;

    int detect_pid_ = -1;
    int proc_pid_ = -1;
    std::string detect_path_;
    std::string out_path_;
    long out_offset_ = 0;
    uint32_t last_detect_ = 0;

    std::array<float, kWidth> peak_{};
    std::array<float, kWidth> smooth_{};
    bool smooth_valid_ = false;
    int iq_fd_ = -1;
    std::string iq_path_;
    std::vector<uint8_t> iq_tail_;
    std::array<uint16_t, 64> palette_{};
    std::vector<uint16_t> pixels_;
    std::deque<std::string> devices_;
    std::string message_;
    uint32_t message_until_ = 0;

    lv_obj_t *title_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *canvas_ = nullptr;
    cp0::ui::Readout centre_;
    cp0::ui::Readout gain_;
    lv_obj_t *edge_low_ = nullptr;
    lv_obj_t *edge_high_ = nullptr;
    lv_obj_t *big_ = nullptr;
    lv_obj_t *sub_ = nullptr;
    lv_obj_t *list_ = nullptr;
    lv_obj_t *listen_ = nullptr;
    lv_obj_t *demod_value_ = nullptr;
    lv_obj_t *state_value_ = nullptr;
    lv_obj_t *audio_pill_ = nullptr;
    lv_obj_t *audio_pill_text_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    void create_ui();
    static uint32_t spectrum_like_color(int index);
    lv_obj_t *make_label(int x, int y, int px, uint32_t color);
    void show_message(const std::string &text);

    void start_detect();
    void poll_detect();
    void set_mode(Mode mode);
    void retune();
    void stop_proc();
    void start_scope();
    void start_decode();
    void toggle_listen();
    std::string tail();

    void read_iq();
    void draw_scope(const std::vector<float> &cols);
    void render();
    void on_timer();
    void handle_key(uint32_t key, bool repeat);
    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
};

/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "../spectrum_scanner.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================
//  Sub-GHz spectrum  UISpectrumPage
//  Content area 320x150 below the top bar:
//    y   0..13   band selector (315 / 433 / 868) and status
//    y  14..63   spectrum trace with peak hold
//    y  65..133  waterfall, newest sweep on top
//    y 137..148  marker frequency and level, key hints (1px bottom margin)
//
//  Keys:
//    TAB, UP/DOWN, 1/2/3   select band (match the antenna on the hat)
//    LEFT/RIGHT            move the marker
//    ENTER                 start / stop a pulse capture at the marker frequency
//    - / =                 lower / raise the capture threshold by 3 dB
//    R                     start / stop recording to ~/spectrum/*.csv
//    P                     open the send list; replay a saved .sub capture
//    SPACE                 pause / resume sweeping
//    H                     clear peak hold
//    ESC                   back to home
//
//  Send list (P): UP/DOWN pick a saved capture, ENTER transmits it on 433 or
//  868 MHz at or below the licence-exempt power limit, ESC returns. Captures
//  outside those bands are marked and refused.
// ============================================================
class UISpectrumPage : public AppPage
{
public:
    UISpectrumPage();
    ~UISpectrumPage() override;

private:
    static constexpr int kWidth = 320;
    static constexpr int kCanvasTop = 14;
    static constexpr int kCanvasH = 120;
    static constexpr int kTraceH = 50;
    static constexpr int kWaterfallTop = kTraceH + 1;
    // Departure Mono ink sits in rows 2-13 of the label, so y=135 leaves the
    // bottom pixel row of the 150px content area clear.
    static constexpr int kFooterY = 135;

    SpectrumScanner scanner_;
    bool scanner_ok_ = false;
    std::size_t band_ = 0;
    int marker_ = spectrum::kBins / 2;
    uint32_t shown_sequence_ = 0;
    std::array<int8_t, spectrum::kBins> latest_{};
    std::array<int8_t, spectrum::kBins> peak_{};
    std::array<int, spectrum::kBands.size()> band_peak_{};
    std::array<uint16_t, spectrum::kLevels> palette_{};
    std::vector<uint16_t> pixels_;
    uint32_t shown_capture_sequence_ = 0;
    bool capture_view_ = false;
    bool band_changed_ = false;
    CaptureStatus capture_;
    std::string message_;
    uint32_t message_until_ = 0;

    struct SendEntry {
        std::string path;
        std::string label;
        uint32_t freq_hz = 0;
        bool allowed = false;
    };
    bool send_view_ = false;
    int send_selection_ = 0;
    std::vector<SendEntry> send_entries_;

    lv_obj_t *canvas_ = nullptr;
    std::array<lv_obj_t *, spectrum::kBands.size()> band_labels_{};
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *marker_label_ = nullptr;
    lv_obj_t *hint_label_ = nullptr;
    lv_obj_t *list_label_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    void create_ui();
    lv_obj_t *make_label(int x, int y, uint32_t color);
    void select_band(std::size_t index);
    void reset_view();
    void enter_send_view();
    void exit_send_view();
    void build_send_entries();
    void refresh_send_list();
    void on_timer();
    void draw_trace();
    void push_waterfall_row();
    void draw_capture(const std::vector<int32_t> &durations);
    void update_labels();
    void show_message(const std::string &text);

    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
    void handle_key(uint32_t key);
};

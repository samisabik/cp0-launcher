/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "../model/keys_model.hpp"
#include "../spectrum_scanner.hpp"
#include "cp0_theme.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================
//  Keys  UIKeysPage
//  Capture inspector for the CC1101: decodes a saved ~/spectrum/*.sub capture
//  to bits and hex, draws its waveform and replays it.
//
//  Keys:
//    UP / DOWN      pick a capture
//    LEFT / RIGHT   scroll the waveform
//    Z              zoom (packet, x2, x4, whole capture)
//    ENTER          send the selected capture
//    R              start / stop a capture on the selected capture's
//                   frequency, 433.92 MHz when there is none
//    ESC            home
// ============================================================
class UIKeysPage : public AppPage
{
public:
    UIKeysPage();
    ~UIKeysPage() override;

private:
    static constexpr int kWaveX = 10;
    static constexpr int kWaveY = 68;
    static constexpr int kWaveW = 300;
    static constexpr int kWaveH = 24;
    static constexpr int kBitsY = 96;
    static constexpr int kGlyphW = 7;
    static constexpr int kBitLabels = kWaveW / kGlyphW;
    static constexpr int kRows = 2;
    static constexpr uint32_t kDefaultHz = 433920000u;

    struct Entry {
        std::string path;
        std::string label;
        uint32_t freq_hz = 0;
        bool allowed = false;
        std::vector<int32_t> durations;
        keys::Analysis analysis;
    };

    SpectrumScanner scanner_;
    bool scanner_ok_ = false;
    std::string scanner_error_;
    std::vector<Entry> entries_;
    int selection_ = 0;

    // What the readouts and the waveform show: the selected capture, or the
    // newest packet while a capture is running.
    std::vector<int32_t> durations_;
    keys::Analysis analysis_;
    int zoom_ = 0;
    uint32_t view_start_us_ = 0;

    bool was_capturing_ = false;
    uint32_t live_sequence_ = 0;
    uint32_t rescan_at_ = 0;
    CaptureStatus capture_;
    std::string message_;
    uint32_t message_until_ = 0;
    bool message_error_ = false;

    std::vector<uint16_t> pixels_;
    lv_obj_t *status_ = nullptr;
    cp0::ui::Readout code_;
    cp0::ui::Readout te_;
    lv_obj_t *canvas_ = nullptr;
    std::array<lv_obj_t *, kBitLabels> bit_labels_{};
    lv_obj_t *info_ = nullptr;
    std::array<lv_obj_t *, kRows> row_boxes_{};
    std::array<lv_obj_t *, kRows> row_labels_{};
    lv_obj_t *empty_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    void create_ui();
    void build_entries();
    void select(int index);
    void show_durations(std::vector<int32_t> durations);
    uint32_t view_span() const;
    void set_zoom(int level);
    void scroll(int direction);
    void show_message(const std::string &text, bool error = false);

    void render();
    void render_readouts();
    void render_list();
    void render_status();
    void draw_wave();

    void on_timer();
    void handle_key(uint32_t key, bool repeat);
    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
};

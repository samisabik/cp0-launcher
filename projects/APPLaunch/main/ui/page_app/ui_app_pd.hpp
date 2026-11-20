/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "../model/audio_model.hpp"
#include "../model/pd_model.hpp"
#include "cp0_theme.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================
//  Pd  UIPdPage
//  Pure Data patch launcher. Lists the patches in ~/pd and runs the selected
//  one in `pd -nogui` as a JACK client of PipeWire, with the microphone on
//  adc~ and the speaker on dac~. While a patch runs the page shows what it
//  prints and the spectrum and level of the sound output.
//
//  Keys:
//    UP / DOWN      pick a patch
//    ENTER          run the selected patch, or stop it when it is running
//    S              stop
//    R              read ~/pd again
//    ESC            stop and go home
// ============================================================
class UIPdPage : public AppPage
{
public:
    UIPdPage();
    ~UIPdPage() override;

private:
    static constexpr int kRows = 9;
    static constexpr int kNameColumns = 13;
    static constexpr int kBands = 62;
    static constexpr int kBandW = 3;
    static constexpr int kCanvasX = 128;
    static constexpr int kCanvasY = 21;
    static constexpr int kCanvasW = kBands * kBandW;
    static constexpr int kCanvasH = 56;
    static constexpr int kConsoleRows = 5;
    static constexpr int kConsoleColumns = 26;

    enum class State { Idle, Starting, Running };

    std::string dir_;
    std::vector<std::string> patches_;
    int selection_ = 0;
    int running_ = -1;
    State state_ = State::Idle;
    bool tools_ok_ = false;
    uint32_t started_ms_ = 0;

    int pd_pid_ = -1;
    int out_fd_ = -1;
    std::string out_path_;
    pd::Console console_{kConsoleColumns, 200};

    int mon_pid_ = -1;
    int mon_fd_ = -1;
    std::string mon_path_;
    bool odd_byte_valid_ = false;
    uint8_t odd_byte_ = 0;
    std::vector<int16_t> samples_;
    float level_db_ = audio::kFloorDb;
    std::array<float, kBands> bars_{};

    std::vector<uint16_t> pixels_;
    lv_obj_t *status_ = nullptr;
    std::array<lv_obj_t *, kRows> row_boxes_{};
    std::array<lv_obj_t *, kRows> row_labels_{};
    lv_obj_t *empty_ = nullptr;
    lv_obj_t *canvas_ = nullptr;
    std::array<lv_obj_t *, kConsoleRows> console_labels_{};
    lv_obj_t *footer_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    void create_ui();
    void scan();
    void start(int index);
    void stop(const char *note);
    bool open_fifo(const std::string &path, int &fd);
    void close_fifo(std::string &path, int &fd);
    void read_output();
    void read_monitor();
    void analyse();
    void draw();
    void render();
    void on_timer();
    void handle_key(uint32_t key);

    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
};

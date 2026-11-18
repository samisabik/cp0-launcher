/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "../model/location_model.hpp"
#include "../model/sat_model.hpp"
#include "cp0_theme.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ============================================================
//  Sat  UISatPage
//  Satellite pass predictor with a polar sky plot. Tracks the satellites in
//  ~/sat/tle.txt that match the built-in interest list or ~/sat/watch.txt,
//  for the observer in ~/.config/cp0/location. Passes are computed on a
//  worker thread with the simplified propagator in sat_model (not SGP4).
//
//  Keys:
//    UP / DOWN   select pass
//    U           fetch fresh TLEs with curl
//    ENTER       record the selected satellite's downlink with rtl_fm while
//                it is above the horizon; ENTER again stops
//    A           antenna hint
//    ESC         home
// ============================================================
class UISatPage : public AppPage
{
public:
    UISatPage();
    ~UISatPage() override;

private:
    static constexpr int kCanvasW = 132;
    static constexpr int kCanvasH = 130;
    static constexpr int kCanvasX = 5;
    static constexpr int kCanvasY = 20;
    static constexpr int kCentreX = 66;
    static constexpr int kCentreY = 65;
    static constexpr int kRadius = 60;
    static constexpr int kRows = 5;

    struct PassRow {
        int sat = 0; // index into the satellite list of the same result
        sat::Pass pass;
    };
    enum class Outcome { None, NoTle, NoMatch, Ok };
    struct ArcPoint {
        double time = 0.0;
        double azimuth_deg = 0.0;
        double elevation_deg = 0.0;
    };

    location::Position observer_;
    std::string tle_path_;
    std::string watch_path_;
    bool tle_exists_ = false;
    long long tle_mtime_ = 0;
    long long computed_mtime_ = -1;
    uint32_t last_stat_ = 0;
    uint32_t last_location_ = 0;
    uint32_t last_second_ = 0;
    double last_compute_ = 0.0;

    // The worker reads and parses the TLE file and searches the passes; the
    // LVGL thread takes the finished result under the mutex.
    std::thread worker_;
    std::atomic<bool> worker_busy_{false};
    std::atomic<bool> worker_stop_{false};
    std::mutex result_mutex_;
    bool result_ready_ = false;
    Outcome result_outcome_ = Outcome::None;
    std::vector<sat::Tle> result_sats_;
    std::vector<PassRow> result_passes_;
    bool want_compute_ = false;

    Outcome outcome_ = Outcome::None;
    std::vector<sat::Tle> sats_;
    std::vector<PassRow> passes_;
    int selected_ = 0;
    int scroll_ = 0;
    std::vector<ArcPoint> arc_;
    bool blink_ = false;

    int update_pid_ = -1;
    std::string update_status_path_;
    uint32_t update_started_ = 0;

    int detect_pid_ = -1;
    std::string detect_path_;
    uint32_t detect_started_ = 0;
    int record_pid_ = -1;
    std::string record_path_;
    std::string record_status_path_;
    std::string record_name_;
    uint32_t record_hz_ = 0;
    double record_until_ = 0.0;

    std::vector<uint16_t> pixels_;
    std::string message_;
    uint32_t message_until_ = 0;
    uint32_t popup_until_ = 0;

    lv_obj_t *title_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *canvas_ = nullptr;
    cp0::ui::Readout next_;
    lv_obj_t *select_bar_ = nullptr;
    std::array<lv_obj_t *, kRows> row_labels_{};
    lv_obj_t *empty_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    cp0::ui::Panel popup_;
    std::array<lv_obj_t *, 3> popup_lines_{};
    lv_timer_t *timer_ = nullptr;

    void create_ui();
    void show_message(const std::string &text);
    void show_popup(bool on);

    void load_location();
    void stat_tle();
    void start_worker();
    void worker_main(std::string tle_path, std::string watch_path, sat::Observer observer, double start);
    void take_result();
    void prune_passes(double now);
    void rebuild_arc();
    const PassRow *selected_pass() const;

    void start_update();
    void poll_update();
    void toggle_record();
    void poll_detect();
    void start_record();
    void finish_record(const std::string &why);

    void draw_sky(double now);
    void render(double now);
    void on_timer();
    void handle_key(uint32_t key, bool repeat);
    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
};

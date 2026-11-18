/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "../model/adsb_model.hpp"
#include "../model/location_model.hpp"
#include "cp0_theme.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================
//  ADSB  UIAdsbPage
//  Aircraft radar from 1090 MHz ADS-B frames printed by rtl_adsb: a scope
//  on the left and a flight list sorted by distance on the right. Range and
//  bearing are from ~/.config/cp0/location, or from the centroid of the
//  decoded aircraft when that file is missing.
//
//  Keys:
//    UP / DOWN   select aircraft
//    + / -       range (20 / 40 / 80 / 160 nm)
//    A           antenna hint
//    ESC         home
// ============================================================
class UIAdsbPage : public AppPage
{
public:
    UIAdsbPage();
    ~UIAdsbPage() override;

private:
    static constexpr int kCanvasW = 132;
    static constexpr int kCanvasH = 130;
    static constexpr int kCanvasX = 5;
    static constexpr int kCanvasY = 20;
    static constexpr int kCentreX = 66;
    static constexpr int kCentreY = 65;
    static constexpr int kRadius = 60;
    static constexpr int kRows = 8;

    struct Row {
        uint32_t icao = 0;
        std::string text;
        std::string label;
        bool has_position = false;
        double distance_nm = 0.0;
        double bearing_deg = 0.0;
        bool has_track = false;
        double track_deg = 0.0;
    };

    bool tools_ok_ = false;
    bool present_ = false;
    int detect_pid_ = -1;
    int proc_pid_ = -1;
    std::string detect_path_;
    std::string out_path_;
    long out_offset_ = 0;
    std::string pending_;
    uint32_t last_detect_ = 0;
    uint32_t last_location_ = 0;

    adsb::Table table_;
    location::Position observer_;
    std::vector<Row> rows_;
    int range_index_ = 1;
    bool has_selection_ = false;
    uint32_t selected_icao_ = 0;
    int scroll_ = 0;
    double sweep_ = 0.0;
    std::vector<uint16_t> pixels_;
    std::string message_;
    uint32_t message_until_ = 0;
    uint32_t popup_until_ = 0;

    lv_obj_t *status_ = nullptr;
    lv_obj_t *canvas_ = nullptr;
    lv_obj_t *ring_half_ = nullptr;
    lv_obj_t *ring_full_ = nullptr;
    lv_obj_t *tag_ = nullptr;
    lv_obj_t *select_bar_ = nullptr;
    std::array<lv_obj_t *, kRows> row_labels_{};
    lv_obj_t *empty_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    cp0::ui::Panel popup_;
    lv_timer_t *timer_ = nullptr;

    void create_ui();
    void show_message(const std::string &text);
    void show_popup(bool on);

    void start_detect();
    void poll_detect();
    void start_receiver();
    void stop_receiver();
    void read_frames();
    void load_location();

    int range_nm() const;
    void rebuild_rows();
    void move_selection(int delta);
    void draw_scope();
    void render();
    void on_timer();
    void handle_key(uint32_t key, bool repeat);
    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
};

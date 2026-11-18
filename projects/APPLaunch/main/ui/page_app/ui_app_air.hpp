/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "../model/air_model.hpp"
#include "cp0_theme.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ============================================================
//  Air  UIAirPage
//  Wi-Fi channel map and network list from an nmcli scan, and a list of
//  nearby Bluetooth devices from the platform Bluetooth API. Scan only:
//  the page never connects, disconnects or changes a radio's power.
//
//  Keys:
//    TAB         switch between the wi-fi and ble tabs
//    UP / DOWN   select a network or device
//    5           wi-fi: switch between the 2.4 GHz and 5 GHz views
//    S           wi-fi: cycle the sort (signal / channel / name)
//    ESC         home
// ============================================================
class UIAirPage : public AppPage
{
public:
    UIAirPage();
    ~UIAirPage() override;

private:
    enum class Tab { Wifi, Ble };

    static constexpr int kMapW = 310;
    static constexpr int kMapH = 84;
    static constexpr int kAxisLabels = 13;
    static constexpr int kNameLabels = 3;
    static constexpr int kWifiRows = 3;
    static constexpr int kBleRows = 8;

    // A shell command running in the background. Its stdout, stderr and exit
    // status land in <base>.out, <base>.err and <base>.rc; the .rc file is
    // written last, so its presence means the other two are complete.
    struct Job {
        int pid = -1;
        std::string base;
        uint32_t started = 0;
    };

    // Replies of the Bluetooth API, which may arrive on a worker thread and
    // after the page is gone; the callbacks own a reference to this.
    struct BleReplies {
        std::mutex mutex;
        bool status_seen = false;
        int status_code = 0;
        std::string status;
        bool list_seen = false;
        std::string list;
        bool discovery_failed = false;
    };

    struct ListRow {
        lv_obj_t *bg = nullptr;
        lv_obj_t *text = nullptr;
        cp0::ui::SegBar bar;
    };

    Tab tab_ = Tab::Wifi;
    air::Band band_ = air::Band::G24;
    air::Sort sort_ = air::Sort::Signal;

    bool nmcli_ok_ = false;
    bool wifi_known_ = false;
    std::string wifi_error_;
    std::vector<air::Network> all_;
    std::vector<air::Network> view_;
    std::string wifi_key_;
    int wifi_selected_ = 0;
    int wifi_top_ = 0;
    Job scan_job_;
    uint32_t last_scan_ = 0;

    std::shared_ptr<BleReplies> ble_replies_;
    bool ble_known_ = false;
    bool ble_available_ = false;
    bool ble_powered_ = false;
    bool ble_discovering_ = false;
    bool ble_discovery_failed_ = false;
    std::vector<air::BleDevice> ble_devices_;
    std::string ble_key_;
    int ble_selected_ = 0;
    int ble_top_ = 0;
    uint32_t ble_entered_ = 0;
    uint32_t last_ble_status_ = 0;
    uint32_t last_ble_list_ = 0;

    std::string message_;
    uint32_t message_until_ = 0;

    std::vector<uint16_t> pixels_;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *wifi_view_ = nullptr;
    lv_obj_t *ble_view_ = nullptr;
    lv_obj_t *canvas_ = nullptr;
    std::array<lv_obj_t *, kAxisLabels> axis_{};
    std::array<lv_obj_t *, kNameLabels> names_{};
    lv_obj_t *wifi_big_ = nullptr;
    lv_obj_t *wifi_sub_ = nullptr;
    std::array<ListRow, kWifiRows> wifi_rows_{};
    lv_obj_t *ble_header_ = nullptr;
    lv_obj_t *ble_big_ = nullptr;
    lv_obj_t *ble_sub_ = nullptr;
    std::array<ListRow, kBleRows> ble_rows_{};
    lv_obj_t *hint_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    void create_ui();
    void show_message(const std::string &text);
    void save_view_config();

    static void start_job(Job &job, const std::string &command);
    static bool finish_job(Job &job, uint32_t timeout_ms, int &rc, std::string &out, std::string &err);
    static void cancel_job(Job &job);

    void start_scan();
    void apply_scan(int rc, const std::string &out, const std::string &err);
    void rebuild_view();
    static std::string key_of(const air::Network &network);

    void enter_ble();
    void leave_ble();
    void request_ble_status();
    void request_ble_list();
    bool take_ble_replies();

    void set_tab(Tab tab);
    void move_selection(int delta);

    void plot(int x, int y, uint16_t color);
    void line(int x0, int y0, int x1, int y1, uint16_t color);
    void draw_map();
    void render();
    void on_timer();
    void handle_key(uint32_t key, bool repeat);
    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
};

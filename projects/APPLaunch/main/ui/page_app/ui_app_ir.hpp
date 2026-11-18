/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "../model/ir_model.hpp"
#include "cp0_theme.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ============================================================
//  IR  UIIrPage
//  Learns an infrared remote button by button and replays it, through the
//  kernel LIRC character devices (/dev/lirc0../dev/lirc3). The receiver and
//  the emitter are found by their LIRC features, so either can be missing.
//  Remotes are ~/ir/remote-<n>.txt, n = 1..4.
//
//  Keys:
//    arrows         select a button tile
//    L              learn the selected tile (L or ESC cancels)
//    ENTER          send the selected tile
//    DEL            clear the selected tile
//    TAB            next remote
//    ESC            home
// ============================================================
class UIIrPage : public AppPage
{
public:
    UIIrPage();
    ~UIIrPage() override;

private:
    static constexpr int kBurstW = 300;
    static constexpr int kBurstH = 13;
    static constexpr int kColumns = 4;

    struct Tile {
        cp0::ui::Panel panel;
        lv_obj_t *name = nullptr;
        lv_obj_t *code = nullptr;
    };

    ir::Remote remote_;
    int remote_number_ = 1;
    int selected_ = 0;
    int learn_slot_ = -1;
    ir::Durations last_frame_;
    ir::Code last_code_;
    int repeat_count_ = 0;
    std::string message_;
    uint32_t message_until_ = 0;

    // The worker owns the device descriptors. It hands received frames and
    // device state to the LVGL thread, and takes transmit requests from it,
    // all under mutex_.
    std::thread worker_;
    std::atomic<bool> worker_stop_{false};
    std::mutex mutex_;
    std::deque<ir::Durations> frames_;
    std::vector<uint32_t> tx_pending_;
    uint32_t tx_carrier_ = ir::kDefaultCarrierHz;
    std::string tx_result_;
    std::string shared_rx_error_;
    bool shared_rx_open_ = false;
    bool shared_scanned_ = false;

    bool rx_open_ = false;
    bool scanned_ = false;
    std::string rx_error_;

    std::vector<uint16_t> pixels_;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *info_ = nullptr;
    lv_obj_t *canvas_ = nullptr;
    Tile tiles_[ir::kSlotCount];
    lv_obj_t *hint_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    void create_ui();
    void show_message(const std::string &text);
    void load_remote();
    bool save_remote();
    void learn(const ir::Durations &frame, const ir::Code &code);
    void send_selected();
    void worker_loop();

    void draw_burst();
    void render();
    void on_timer();
    void handle_key(uint32_t key, bool repeat);
    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
};

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
//  Full-bleed 320x170, no top bar. Two tabs cycled with TAB:
//    RX   live finder: per-channel level bars vs a fixed trigger; a hit
//         auto-captures that channel to ~/spectrum/*.sub + *.ook.
//    TX   saved captures; ENTER replays one on its own frequency.
//
//  Keys:
//    TAB            next tab
//    ESC            home
//    RX   1/2/3 antenna, UP/DOWN trigger, A auto, B filter, ENTER capture
//    TX   UP/DOWN pick, ENTER send, DEL delete
// ============================================================
class UISpectrumPage : public AppPage
{
public:
    UISpectrumPage();
    ~UISpectrumPage() override;

private:
    enum class Tab { Rx, Tx };

    static constexpr int kWidth = 320;
    static constexpr int kTabTop = 0;
    static constexpr int kTabH = 15;
    static constexpr int kBodyTop = 17;
    static constexpr int kBodyH = 136; // 17..153
    static constexpr int kHintY = 156;

    SpectrumScanner scanner_;
    bool scanner_ok_ = false;
    std::size_t band_ = 1; // fitted antenna: index into kBands (default 433)
    uint32_t last_hit_hz_ = 433920000;
    CaptureStatus capture_;
    std::string message_;
    uint32_t message_until_ = 0;

    Tab tab_ = Tab::Rx;
    uint32_t shown_finder_sequence_ = 0;

    struct SendEntry {
        std::string path;
        std::string label;
        uint32_t freq_hz = 0;
        bool allowed = false;
    };
    int send_selection_ = 0;
    std::vector<SendEntry> send_entries_;

    // Tab bar
    std::array<lv_obj_t *, 2> tab_labels_{};
    lv_obj_t *tab_hl_ = nullptr;

    // RX
    lv_obj_t *rx_panel_ = nullptr;
    std::array<lv_obj_t *, spectrum::kFinderChannels.size()> finder_freq_{};
    std::array<lv_obj_t *, spectrum::kFinderChannels.size()> finder_bar_{};
    std::array<lv_obj_t *, spectrum::kFinderChannels.size()> finder_value_{};
    lv_obj_t *hit_title_ = nullptr;
    lv_obj_t *hit_freq_ = nullptr;
    lv_obj_t *hit_detail_ = nullptr;
    lv_obj_t *hit_history_ = nullptr;
    lv_obj_t *rx_status_ = nullptr;

    // TX
    lv_obj_t *tx_panel_ = nullptr;
    lv_obj_t *tx_list_ = nullptr;
    lv_obj_t *tx_detail_ = nullptr;

    lv_obj_t *hint_label_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    lv_obj_t *mk_label(lv_obj_t *parent, int x, int y, int px, uint32_t color);
    void create_tabbar();
    void create_rx();
    void create_tx();
    void select_tab(Tab tab);
    void select_band(std::size_t index);
    void build_send_entries();
    void refresh_tx_list();
    void delete_selected();
    void render_rx();
    void update_hint();
    void on_timer();
    void show_message(const std::string &text);
    void handle_key(uint32_t key);

    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
};

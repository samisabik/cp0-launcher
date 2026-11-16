/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "../spectrum_scanner.hpp"
#include "cp0_theme.hpp"

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
    static constexpr int kTabX = 110;
    static constexpr int kBarX = 52;
    static constexpr int kBarW = 222;
    static constexpr int kTxRows = 4;
    static constexpr int kBodyTop = 17;
    static constexpr int kBodyH = 136; // 17..153

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
        int pulses = 0;
        uint32_t duration_ms = 0;
        long mtime = 0;
    };
    int send_selection_ = 0;
    std::vector<SendEntry> send_entries_;

    // Tab bar
    std::array<lv_obj_t *, 2> tab_labels_{};
    std::array<lv_obj_t *, 2> tab_lines_{};
    lv_obj_t *status_ = nullptr;

    // RX
    lv_obj_t *rx_panel_ = nullptr;
    std::array<lv_obj_t *, spectrum::kFinderChannels.size()> finder_freq_{};
    std::array<cp0::ui::SegBar, spectrum::kFinderChannels.size()> finder_bar_{};
    std::array<lv_obj_t *, spectrum::kFinderChannels.size()> finder_value_{};
    cp0::ui::Readout hit_;
    cp0::ui::Readout peak_;
    lv_obj_t *mode_ = nullptr;
    lv_obj_t *trigger_line_ = nullptr;
    lv_obj_t *trigger_label_ = nullptr;

    // TX
    struct TxRow {
        lv_obj_t *row = nullptr;
        lv_obj_t *mark = nullptr;
        lv_obj_t *time = nullptr;
        lv_obj_t *freq = nullptr;
        lv_obj_t *pulses = nullptr;
    };
    lv_obj_t *tx_panel_ = nullptr;
    cp0::ui::Readout tx_freq_;
    lv_obj_t *tx_signal_ = nullptr;
    lv_obj_t *tx_length_ = nullptr;
    lv_obj_t *tx_empty_ = nullptr;
    std::array<TxRow, kTxRows> tx_rows_{};

    // Rename
    bool renaming_ = false;
    std::string rename_text_;
    cp0::ui::Panel rename_panel_;
    lv_obj_t *rename_value_ = nullptr;
    lv_obj_t *rename_cursor_ = nullptr;
    bool input_context_saved_ = false;
    int previous_input_context_ = 0;
    int previous_keypad_intercept_ = 0;

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
    void start_rename();
    void finish_rename(bool save);
    void render_rename();
    void handle_rename_key(uint32_t key, const struct key_item *item);
    void render_rx();
    void update_hint();
    void on_timer();
    void show_message(const std::string &text);
    void handle_key(uint32_t key);

    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
};

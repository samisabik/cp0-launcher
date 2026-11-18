/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "../model/tracker_model.hpp"
#include "cp0_keyboard_input_context.hpp"
#include "cp0_theme.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

// ============================================================
//  Tracker  UITrackerPage
//  Four-channel chiptune tracker: two squares, a wavetable and noise,
//  played through pacat. Shows "no audio" and still edits without it.
//  The page reads physical keys, so Z / X / C are notes here and the
//  arrow keys are the only way to move.
//
//  Keys:
//    arrows         move the cursor (left/right channel, up/down row)
//    SPACE          play / stop from the top of the current pattern
//    Z S X D C V G B H N J M     notes C..B in the current octave
//    Q 2 W 3 E R 5 T 6 Y 7 U     notes C..B one octave up
//    [ / ]          octave down / up
//    . or 1         note-off
//    DEL / BKSP     clear the cell
//    ENTER          hear the cell under the cursor
//    TAB            cycle the instrument (duty / wave / noise mode) of the channel
//    9 / 0          volume down / up
//    ; / '          transpose the cell a semitone down / up
//    I / K          insert / delete a row in the channel
//    A              cycle the decay envelope of the channel
//    - / =          previous / next pattern
//    8 / 4          append the pattern to the order list / drop its last entry
//    , / /          tempo down / up
//    O              save ~/tracker/song.txt
//    P              export ~/tracker/song.wav
//    ESC            home
// ============================================================
class UITrackerPage : public AppPage
{
public:
    UITrackerPage();
    ~UITrackerPage() override;

private:
    static constexpr int kVisibleRows = 9;
    static constexpr int kCentreRow = 4;

    struct Column {
        cp0::ui::Panel panel;
        lv_obj_t *name = nullptr;
        cp0::ui::SegBar meter;
        std::array<lv_obj_t *, kVisibleRows> number{};
        std::array<lv_obj_t *, kVisibleRows> cell{};
    };

    Cp0KeyboardInputContextScope input_context_{KBD_INPUT_CONTEXT_TEXT};

    int pattern_ = 0;
    int row_ = 0;
    int channel_ = 0;
    int octave_ = 4;
    int volume_ = 12;
    std::array<int, tracker::kChannels> inst_ = {2, 1, 0, 0};
    bool was_playing_ = false;
    std::string message_;
    uint32_t message_until_ = 0;

    bool tool_ok_ = false;
    bool no_audio_ = false;
    int audio_pid_ = -1;
    int spawn_count_ = 0;
    uint32_t last_spawn_ = 0;
    std::string audio_path_;
    std::string song_dir_;

    // song_ and player_ are shared with the audio worker under song_mutex_.
    std::mutex song_mutex_;
    tracker::Song song_;
    tracker::Player player_;
    std::thread worker_;
    std::atomic<bool> worker_stop_{false};
    std::atomic<bool> audio_ok_{false};
    std::atomic<bool> audio_dead_{false};
    std::atomic<bool> playing_{false};
    std::atomic<int> play_pattern_{0};
    std::atomic<int> play_row_{0};
    std::array<std::atomic<float>, tracker::kChannels> level_{};

    std::thread export_thread_;
    // 0 idle, 1 running, 2 written, 3 failed
    std::atomic<int> export_state_{0};

    std::array<Column, tracker::kChannels> columns_{};
    lv_obj_t *title_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    void create_ui();
    void show_message(const std::string &text);
    void load_song();
    void save_song();
    void export_wav();
    void start_audio();
    void worker_loop();
    bool audio_missing() const;

    void toggle_play();
    void enter_note(int semitone);
    void step_row();
    tracker::Cell &cursor_cell();
    void render();
    void on_timer();
    void handle_key(uint32_t key, bool repeat);
    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
};

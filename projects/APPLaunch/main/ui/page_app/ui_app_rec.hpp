/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"

#include <array>
#include <cstdint>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// ============================================================
//  Rec  UIRecPage
//  Voice recorder with on-device speech to text. Recordings live in
//  ~/Recordings/<stamp>.wav (the folder the old recorder package used);
//  transcripts sit next to them as <stamp>.txt. Transcription runs
//  ffmpeg (16 kHz mono, loudness normalised) then whisper-cli tiny.en,
//  one clip at a time; new recordings are queued automatically.
//
//  List:   SPACE record   ENTER/RIGHT open   T transcribe   D twice delete
//  Clip:   P play/stop    T transcribe       D twice delete  ESC back
//  Recording: SPACE/ENTER stop and transcribe, ESC discard
// ============================================================
class UIRecPage : public AppPage
{
public:
    UIRecPage();
    ~UIRecPage() override;

private:
    enum class View { List, Clip, Recording };
    static constexpr int kRows = 7;

    struct Clip {
        std::string stem;
        uint32_t duration_ms = 0;
        bool has_transcript = false;
        std::string transcript;
        long long wav_size = -1;
        std::time_t txt_mtime = 0;
    };

    // Recordings whose arecord process a worker has finished stopping; the
    // UI thread picks them up to list and transcribe them.
    struct Stopped {
        std::mutex mutex;
        std::vector<std::string> stems;
    };

    View view_ = View::List;
    std::vector<Clip> clips_;
    int selection_ = 0;
    uint32_t delete_armed_until_ = 0;
    std::string message_;
    uint32_t message_until_ = 0;

    int rec_pid_ = -1;
    std::string rec_stem_;
    uint32_t rec_since_ = 0;
    int level_ = 0;

    int stt_pid_ = -1;
    std::string stt_stem_;
    uint32_t stt_since_ = 0;
    uint32_t stt_estimate_ms_ = 0;
    std::deque<std::string> stt_queue_;

    int play_pid_ = -1;
    std::string play_stem_;
    uint32_t play_since_ = 0;

    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *side_label_ = nullptr;
    std::array<lv_obj_t *, kRows> rows_{};
    std::array<lv_obj_t *, kRows> row_meta_{};
    std::array<lv_obj_t *, kRows> row_text_{};
    lv_obj_t *clip_panel_ = nullptr;
    lv_obj_t *clip_text_ = nullptr;
    lv_obj_t *clip_progress_ = nullptr;
    lv_obj_t *rec_panel_ = nullptr;
    lv_obj_t *rec_time_ = nullptr;
    lv_obj_t *rec_meter_ = nullptr;
    lv_obj_t *rec_note_ = nullptr;
    lv_obj_t *footer_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    std::shared_ptr<Stopped> stopped_ = std::make_shared<Stopped>();

    void create_ui();
    lv_obj_t *make_label(lv_obj_t *parent, int x, int y, uint32_t color);
    lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color);

    void load_clips();
    int find_clip(const std::string &stem) const;
    const Clip *selected() const;
    void render();
    void render_list();
    void render_clip();
    void render_recording();
    void set_chips(const std::vector<std::pair<const char *, const char *>> &chips);
    std::string clip_status(const Clip &clip, uint32_t &color) const;
    void show_message(const std::string &text);

    void start_recording();
    void stop_recording(bool keep);
    void take_stopped_recordings();
    void queue_transcription(const std::string &stem);
    void start_next_transcription();
    void finish_transcription();
    void toggle_playback();
    void stop_playback();
    void delete_selected();
    void stop_all();

    void on_timer();
    void handle_key(uint32_t key, bool repeat);
    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
};

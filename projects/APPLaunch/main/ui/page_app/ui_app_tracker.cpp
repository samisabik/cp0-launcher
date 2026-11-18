/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_tracker.hpp"

#include "../app_support.hpp"
#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "input_keys.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint32_t kRefreshMs = 40;
constexpr uint32_t kMessageMs = 2500;
constexpr uint32_t kRespawnMs = 3000;
constexpr uint32_t kAudioGraceMs = 1500;
constexpr int kMaxSpawns = 5;
constexpr int kMaxOctave = 6;
// 10 ms blocks, rendered 60 ms ahead of the clock.
constexpr int kBlockFrames = tracker::kRate / 100;
constexpr int kLeadFrames = tracker::kRate * 60 / 1000;
constexpr float kMeterDecay = 0.8f;

constexpr int kColumnX = 4;
constexpr int kColumnPitch = 79;
constexpr int kColumnY = 19;
constexpr int kColumnW = 75;
constexpr int kColumnH = 132;

struct PianoKey {
    uint32_t key;
    int semitone;
};

// Two rows of keys in the usual tracker layout, C to B, the upper row one octave above.
constexpr PianoKey kPiano[] = {
    {KEY_Z, 0},  {KEY_S, 1},  {KEY_X, 2},  {KEY_D, 3},  {KEY_C, 4},  {KEY_V, 5},  {KEY_G, 6},  {KEY_B, 7},
    {KEY_H, 8},  {KEY_N, 9},  {KEY_J, 10}, {KEY_M, 11}, {KEY_Q, 12}, {KEY_2, 13}, {KEY_W, 14}, {KEY_3, 15},
    {KEY_E, 16}, {KEY_R, 17}, {KEY_5, 18}, {KEY_T, 19}, {KEY_6, 20}, {KEY_Y, 21}, {KEY_7, 22}, {KEY_U, 23},
};

// Punctuation and arrows can sit on a modifier layer of the keyboard, so they
// are matched by what the key produced; letters and digits by the physical key.
uint32_t resolve_key(const struct key_item *item)
{
    if (std::strcmp(item->sym_name, "Up") == 0) return KEY_UP;
    if (std::strcmp(item->sym_name, "Down") == 0) return KEY_DOWN;
    if (std::strcmp(item->sym_name, "Left") == 0) return KEY_LEFT;
    if (std::strcmp(item->sym_name, "Right") == 0) return KEY_RIGHT;
    if (item->utf8[0] != '\0' && item->utf8[1] == '\0') {
        switch (item->utf8[0]) {
        case '[': return KEY_LEFTBRACE;
        case ']': return KEY_RIGHTBRACE;
        case '-': return KEY_MINUS;
        case '=': return KEY_EQUAL;
        case '.': return KEY_DOT;
        case ',': return KEY_COMMA;
        case '/': return KEY_SLASH;
        case ';': return KEY_SEMICOLON;
        case '\'': return KEY_APOSTROPHE;
        default: break;
        }
    }
    return item->key_code;
}

} // namespace

UITrackerPage::UITrackerPage() : AppPage()
{
    set_page_title("Tracker");
    reclaim_top_bar();
    audio_path_ = app_support::runtime_dir() + "/cp0-tracker-audio-" + std::to_string(getpid());
    song_dir_ = app_support::home_dir() + "/tracker";
    for (auto &level : level_) level = 0.0f;
    load_song();
    tool_ok_ = app_support::tool_present("pacat");
    create_ui();
    render();
    start_audio();
    worker_ = std::thread([this] { worker_loop(); });
    timer_ = lv_timer_create(timer_cb, kRefreshMs, this);
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

UITrackerPage::~UITrackerPage()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
    worker_stop_ = true;
    if (worker_.joinable()) worker_.join();
    if (export_thread_.joinable()) export_thread_.join();
    app_support::stop(audio_pid_);
    unlink(audio_path_.c_str());
}

void UITrackerPage::create_ui()
{
    if (!ui_APP_Container) return;
    cp0::ui::paint_background(ui_APP_Container);
    const cp0::ui::TitleRow row = cp0::ui::title_row(ui_APP_Container, "tracker");
    title_ = row.title;
    status_ = row.status;

    for (int c = 0; c < tracker::kChannels; ++c) {
        Column &column = columns_[static_cast<std::size_t>(c)];
        column.panel =
            cp0::ui::panel(ui_APP_Container, kColumnX + c * kColumnPitch, kColumnY, kColumnW, kColumnH);
        lv_obj_t *box = column.panel.box;
        if (!box) continue;
        // The highlight sits under the labels of the middle row.
        cp0::ui::box(box, 1, 68, kColumnW - 2, 12, cp0::theme::kSelect);
        column.name = cp0::ui::label(box, 5, 1, cp0::theme::kDim);
        if (column.name) lv_label_set_text(column.name, tracker::channel_name(c));
        column.meter = cp0::ui::segbar(box, 32, 5, 38, 6);
        cp0::ui::dotted_rule(box, 2, 15, kColumnW - 4);
        for (int r = 0; r < kVisibleRows; ++r) {
            const int y = 17 + r * 25 / 2;
            column.number[static_cast<std::size_t>(r)] = cp0::ui::label(box, 5, y, cp0::theme::kBracket);
            column.cell[static_cast<std::size_t>(r)] = cp0::ui::label(box, 19, y, cp0::theme::kBracket);
        }
    }
    hint_ = cp0::ui::footer(ui_APP_Container);
}

void UITrackerPage::show_message(const std::string &text)
{
    message_ = text;
    message_until_ = lv_tick_get() + kMessageMs;
    if (message_until_ == 0) message_until_ = 1;
    render();
}

void UITrackerPage::load_song()
{
    const std::string text = app_support::read_file(song_dir_ + "/song.txt");
    if (text.empty() || !tracker::from_text(text, song_)) song_ = tracker::demo_song();
    if (!song_.order.empty()) pattern_ = song_.order[0];
}

void UITrackerPage::save_song()
{
    std::string text;
    {
        std::lock_guard<std::mutex> lock(song_mutex_);
        text = tracker::to_text(song_);
    }
    mkdir(song_dir_.c_str(), 0755);
    std::FILE *file = std::fopen((song_dir_ + "/song.txt").c_str(), "w");
    bool ok = file != nullptr;
    if (file) {
        ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
        ok = std::fclose(file) == 0 && ok;
    }
    show_message(ok ? "saved song.txt" : "save failed");
}

void UITrackerPage::export_wav()
{
    if (export_state_ == 1) return;
    if (export_thread_.joinable()) export_thread_.join();
    tracker::Song copy;
    {
        std::lock_guard<std::mutex> lock(song_mutex_);
        copy = song_;
    }
    mkdir(song_dir_.c_str(), 0755);
    const std::string path = song_dir_ + "/song.wav";
    export_state_ = 1;
    // A long order list is several megabytes of audio, so it renders off the LVGL thread.
    export_thread_ = std::thread([this, copy, path] {
        bool ok = false;
        try {
            const std::vector<uint8_t> wav = tracker::wav_bytes(tracker::render_song(copy));
            if (std::FILE *file = std::fopen(path.c_str(), "wb")) {
                ok = std::fwrite(wav.data(), 1, wav.size(), file) == wav.size();
                ok = std::fclose(file) == 0 && ok;
            }
        } catch (...) {
        }
        export_state_ = ok ? 2 : 3;
    });
    show_message("exporting...");
}

void UITrackerPage::start_audio()
{
    if (!tool_ok_ || spawn_count_ >= kMaxSpawns) return;
    app_support::stop(audio_pid_);
    last_spawn_ = lv_tick_get();
    if (spawn_count_ == 0) {
        unlink(audio_path_.c_str());
        if (mkfifo(audio_path_.c_str(), 0600) != 0) {
            spawn_count_ = kMaxSpawns;
            return;
        }
    }
    // --latency-msec is unverified on the device (no pacat in the sysroot to
    // check against), so every retry after the first start runs without it.
    const std::string latency = spawn_count_ == 0 ? " --latency-msec=60" : "";
    ++spawn_count_;
    audio_dead_ = false;
    audio_pid_ = app_support::spawn("exec pacat --raw --rate=" + std::to_string(tracker::kRate) +
                                    " --channels=1 --format=s16le" + latency + " < '" + audio_path_ + "'");
}

void UITrackerPage::worker_loop()
{
    // SIGPIPE from a write is delivered to the writing thread; blocked here,
    // a dead pacat turns into EPIPE instead of killing the launcher.
    sigset_t sigpipe;
    sigemptyset(&sigpipe);
    sigaddset(&sigpipe, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &sigpipe, nullptr);

    using clock = std::chrono::steady_clock;
    int fd = -1;
    bool running = false;
    clock::time_point start;
    int64_t written = 0;
    std::array<float, tracker::kChannels> meter{};
    int16_t block[kBlockFrames];

    auto lost = [&] {
        close(fd);
        fd = -1;
        audio_ok_ = false;
        audio_dead_ = true;
    };

    while (!worker_stop_) {
        // pacat opens the pipe for reading once it starts; until then a
        // non-blocking writer open fails and is retried.
        if (fd < 0 && tool_ok_ && !audio_dead_) {
            fd = open(audio_path_.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0) audio_ok_ = true;
        }
        if (fd >= 0) {
            // poll(2) documents POLLERR on the write end of a pipe whose read
            // end is closed, which catches a dead pacat while nothing is written.
            pollfd pfd{fd, POLLOUT, 0};
            if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLERR)) lost();
        }

        bool silent = false;
        {
            std::lock_guard<std::mutex> lock(song_mutex_);
            silent = player_.silent();
        }
        if (silent) {
            // Nothing is written while silent, so pacat drains and the next
            // note starts a fresh time base without accumulated drift.
            running = false;
            playing_ = false;
            for (std::size_t c = 0; c < meter.size(); ++c) {
                meter[c] *= kMeterDecay;
                level_[c] = meter[c];
            }
            usleep(10000);
            continue;
        }

        const clock::time_point now = clock::now();
        if (!running) {
            running = true;
            start = now;
            written = 0;
        }
        const int64_t elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(now - start).count() * tracker::kRate / 1000000;
        const int64_t target = elapsed + kLeadFrames;
        // After a stall, skip ahead instead of rendering the backlog in a burst.
        if (target - written > tracker::kRate / 4) written = target - kLeadFrames;
        while (written < target && !worker_stop_) {
            const int count = static_cast<int>(std::min<int64_t>(target - written, kBlockFrames));
            {
                std::lock_guard<std::mutex> lock(song_mutex_);
                player_.render(song_, block, count);
                playing_ = player_.playing();
                play_pattern_ = player_.pattern();
                play_row_ = player_.row();
                for (std::size_t c = 0; c < meter.size(); ++c) {
                    meter[c] = std::max(player_.level(static_cast<int>(c)), meter[c] * kMeterDecay);
                    level_[c] = meter[c];
                }
            }
            written += count;
            // A block is far below PIPE_BUF, so a non-blocking write is all or
            // nothing; a full pipe drops the block.
            if (fd >= 0 && write(fd, block, static_cast<std::size_t>(count) * sizeof(int16_t)) < 0 && errno == EPIPE)
                lost();
        }
        usleep(5000);
    }
    if (fd >= 0) close(fd);
}

bool UITrackerPage::audio_missing() const
{
    return !tool_ok_ || (!audio_ok_ && lv_tick_get() - last_spawn_ >= kAudioGraceMs);
}

void UITrackerPage::toggle_play()
{
    std::lock_guard<std::mutex> lock(song_mutex_);
    if (player_.playing()) {
        player_.stop();
        playing_ = false;
    } else {
        player_.play(song_, pattern_);
        play_pattern_ = pattern_;
        play_row_ = 0;
        playing_ = true;
    }
}

// Call with song_mutex_ held.
tracker::Cell &UITrackerPage::cursor_cell()
{
    return song_.patterns[static_cast<std::size_t>(pattern_)]
        .rows[static_cast<std::size_t>(row_)][static_cast<std::size_t>(channel_)];
}

void UITrackerPage::step_row()
{
    if (!playing_) row_ = (row_ + 1) % tracker::kRows;
}

void UITrackerPage::enter_note(int semitone)
{
    const int note = octave_ * 12 + semitone + 1;
    if (note > tracker::kMaxNote) return;
    {
        std::lock_guard<std::mutex> lock(song_mutex_);
        const int inst = inst_[static_cast<std::size_t>(channel_)];
        if (!tracker::set_note(song_, pattern_, row_, channel_, note, inst, volume_)) return;
        player_.preview(channel_, cursor_cell(), song_.envelope[static_cast<std::size_t>(channel_)]);
    }
    step_row();
    render();
}

void UITrackerPage::render()
{
    tracker::Pattern pattern;
    int bpm = 0;
    {
        std::lock_guard<std::mutex> lock(song_mutex_);
        pattern = song_.patterns[static_cast<std::size_t>(pattern_)];
        bpm = song_.bpm;
    }
    const bool playing = playing_;

    char text[48];
    std::snprintf(text, sizeof(text), "tracker  o%d v%X", octave_, static_cast<unsigned>(volume_));
    cp0::ui::set_text(title_, text, cp0::theme::kText);

    if (status_) {
        if (message_until_ != 0) {
            cp0::ui::set_text(status_, message_.c_str(), cp0::theme::kText);
        } else if (audio_missing()) {
            std::snprintf(text, sizeof(text), "pat %02d  %d no audio", pattern_, bpm);
            cp0::ui::set_text(status_, text, cp0::theme::kWarn);
        } else {
            std::snprintf(text, sizeof(text), "pat %02d  %d bpm", pattern_, bpm);
            cp0::ui::set_text(status_, text, playing ? cp0::theme::kAccent : cp0::theme::kIdle);
        }
    }

    for (int c = 0; c < tracker::kChannels; ++c) {
        const Column &column = columns_[static_cast<std::size_t>(c)];
        const bool focus = c == channel_;
        cp0::ui::set_brackets(column.panel.corners, focus ? cp0::theme::kAccent : cp0::theme::kBracket);
        cp0::ui::set_text_color(column.name, focus ? cp0::theme::kText : cp0::theme::kDim);
        for (int r = 0; r < kVisibleRows; ++r) {
            const int index = (row_ + r - kCentreRow + tracker::kRows) % tracker::kRows;
            const tracker::Cell &cell = pattern.rows[static_cast<std::size_t>(index)][static_cast<std::size_t>(c)];
            const bool current = r == kCentreRow;
            const bool used = cell.note != tracker::kEmpty;
            const char number[2] = {"0123456789ABCDEF"[index], '\0'};
            cp0::ui::set_text(column.number[static_cast<std::size_t>(r)], number,
                              current ? cp0::theme::kAccent : index % 4 ? cp0::theme::kBracket : cp0::theme::kDim);
            uint32_t color = used ? cp0::theme::kText : cp0::theme::kBracket;
            if (current && focus) color = cp0::theme::kAccent;
            cp0::ui::set_text(column.cell[static_cast<std::size_t>(r)], tracker::cell_text(cell).c_str(), color);
        }
    }

    cp0::ui::set_footer(hint_, {{"spc", playing ? "stop" : "play"}, {"tab", "inst"}, {"[ ]", "oct"}, {"- =", "pat"},
                                {"o", "save"}});
}

void UITrackerPage::timer_cb(lv_timer_t *timer) noexcept
{
    auto *page = static_cast<UITrackerPage *>(lv_timer_get_user_data(timer));
    if (!page) return;
    try {
        page->on_timer();
    } catch (...) {
    }
}

void UITrackerPage::on_timer()
{
    bool dirty = false;
    const bool playing = playing_;
    if (playing) {
        // The view follows playback, so notes entered while playing land on the playing row.
        const int pattern = play_pattern_;
        const int row = play_row_;
        dirty = pattern != pattern_ || row != row_;
        pattern_ = pattern;
        row_ = row;
    }
    dirty = dirty || playing != was_playing_;
    was_playing_ = playing;

    for (int c = 0; c < tracker::kChannels; ++c)
        cp0::ui::set_segbar(columns_[static_cast<std::size_t>(c)].meter, level_[static_cast<std::size_t>(c)],
                            cp0::theme::kAccent);

    if (audio_dead_ && lv_tick_get() - last_spawn_ >= kRespawnMs) start_audio();

    const int exported = export_state_;
    if (exported >= 2) {
        export_state_ = 0;
        if (export_thread_.joinable()) export_thread_.join();
        show_message(exported == 2 ? "saved song.wav" : "export failed");
    }
    if (message_until_ != 0 && static_cast<int32_t>(lv_tick_get() - message_until_) >= 0) {
        message_until_ = 0;
        dirty = true;
    }
    // The "no audio" status appears once the grace period after a start is over.
    const bool no_audio = audio_missing();
    dirty = dirty || no_audio != no_audio_;
    no_audio_ = no_audio;
    if (dirty) render();
}

void UITrackerPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UITrackerPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item) return;
    // Ctrl and Alt chords belong to launcher-wide shortcuts such as the screenshot.
    if (item->mods & (KBD_MOD_CTRL | KBD_MOD_ALT)) return;
    try {
        page->handle_key(resolve_key(item), item->key_state == KBD_KEY_REPEATED);
    } catch (...) {
    }
}

void UITrackerPage::handle_key(uint32_t key, bool repeat)
{
    const bool repeats = key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN ||
                         key == KEY_COMMA || key == KEY_SLASH;
    if (repeat && !repeats) return;
    const std::size_t ch = static_cast<std::size_t>(channel_);

    switch (key) {
    case KEY_ESC:
        if (navigate_home) navigate_home();
        return;
    case KEY_LEFT:
    case KEY_RIGHT:
        channel_ = (channel_ + (key == KEY_RIGHT ? 1 : tracker::kChannels - 1)) % tracker::kChannels;
        break;
    case KEY_UP:
    case KEY_DOWN:
        if (playing_) return;
        row_ = (row_ + (key == KEY_DOWN ? 1 : tracker::kRows - 1)) % tracker::kRows;
        break;
    case KEY_SPACE:
        toggle_play();
        break;
    case KEY_LEFTBRACE:
    case KEY_RIGHTBRACE:
        octave_ = std::clamp(octave_ + (key == KEY_RIGHTBRACE ? 1 : -1), 0, kMaxOctave);
        break;
    case KEY_MINUS:
    case KEY_EQUAL: {
        if (playing_) {
            std::lock_guard<std::mutex> lock(song_mutex_);
            player_.stop();
            playing_ = false;
        }
        pattern_ = std::clamp(pattern_ + (key == KEY_EQUAL ? 1 : -1), 0, tracker::kMaxPatterns - 1);
        row_ = 0;
        break;
    }
    case KEY_TAB: {
        inst_[ch] = (inst_[ch] + 1) % tracker::kInstrumentCount[ch];
        {
            std::lock_guard<std::mutex> lock(song_mutex_);
            tracker::Cell &cell = cursor_cell();
            if (cell.note != tracker::kEmpty && cell.note != tracker::kOff) {
                cell.inst = static_cast<uint8_t>(inst_[ch]);
                player_.preview(channel_, cell, song_.envelope[ch]);
            }
        }
        show_message(tracker::instrument_name(channel_, inst_[ch]));
        return;
    }
    case KEY_9:
    case KEY_0: {
        volume_ = std::clamp(volume_ + (key == KEY_0 ? 1 : -1), 1, tracker::kMaxVolume);
        std::lock_guard<std::mutex> lock(song_mutex_);
        tracker::Cell &cell = cursor_cell();
        if (cell.note != tracker::kEmpty && cell.note != tracker::kOff) cell.vol = static_cast<uint8_t>(volume_);
        break;
    }
    case KEY_DOT:
    case KEY_1: {
        {
            std::lock_guard<std::mutex> lock(song_mutex_);
            tracker::set_note_off(song_, pattern_, row_, channel_);
        }
        step_row();
        break;
    }
    case KEY_DELETE:
    case KEY_BACKSPACE: {
        std::lock_guard<std::mutex> lock(song_mutex_);
        tracker::clear_cell(song_, pattern_, row_, channel_);
        break;
    }
    case KEY_ENTER:
    case KEY_KPENTER: {
        std::lock_guard<std::mutex> lock(song_mutex_);
        player_.preview(channel_, cursor_cell(),
                        song_.envelope[ch]);
        return;
    }
    case KEY_SEMICOLON:
    case KEY_APOSTROPHE: {
        std::lock_guard<std::mutex> lock(song_mutex_);
        if (tracker::transpose_cell(song_, pattern_, row_, channel_, key == KEY_APOSTROPHE ? 1 : -1))
            player_.preview(channel_,
                            cursor_cell(),
                            song_.envelope[ch]);
        break;
    }
    case KEY_I:
    case KEY_K: {
        std::lock_guard<std::mutex> lock(song_mutex_);
        if (key == KEY_I)
            tracker::insert_row(song_, pattern_, row_, channel_);
        else
            tracker::delete_row(song_, pattern_, row_, channel_);
        break;
    }
    case KEY_A: {
        int envelope = 0;
        {
            std::lock_guard<std::mutex> lock(song_mutex_);
            song_.envelope[ch] = static_cast<uint8_t>((song_.envelope[ch] + 1) % (tracker::kMaxEnvelope + 1));
            envelope = song_.envelope[ch];
        }
        show_message(envelope ? "decay " + std::to_string(envelope) : std::string("decay off"));
        return;
    }
    case KEY_8:
    case KEY_4: {
        std::string text = "order";
        {
            std::lock_guard<std::mutex> lock(song_mutex_);
            if (key == KEY_8)
                tracker::append_order(song_, pattern_);
            else
                tracker::pop_order(song_);
            // The status holds 20 characters, so a long list shows its end.
            for (uint8_t entry : song_.order) text += " " + std::to_string(entry);
        }
        if (text.size() > 20) text = "order .." + text.substr(text.size() - 12);
        show_message(text);
        return;
    }
    case KEY_COMMA:
    case KEY_SLASH: {
        std::lock_guard<std::mutex> lock(song_mutex_);
        song_.bpm = tracker::clamp_bpm(song_.bpm + (key == KEY_SLASH ? 1 : -1));
        break;
    }
    case KEY_O:
        save_song();
        return;
    case KEY_P:
        export_wav();
        return;
    default:
        for (const PianoKey &piano : kPiano) {
            if (piano.key == key) {
                enter_note(piano.semitone);
                return;
            }
        }
        return;
    }
    render();
}

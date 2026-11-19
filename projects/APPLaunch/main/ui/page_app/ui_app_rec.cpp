/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_rec.hpp"

#include "../model/rec_model.hpp"
#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "input_keys.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <functional>
#include <thread>

namespace {

static const uint32_t &kColorAccent = cp0::theme::kAccent;
static const uint32_t &kColorText = cp0::theme::kText;
static const uint32_t &kColorIdle = cp0::theme::kIdle;
static const uint32_t &kColorDim = cp0::theme::kDim;
static const uint32_t &kColorTrack = cp0::theme::kLine;
static const uint32_t &kColorSelect = cp0::theme::kSelect;
static const uint32_t &kColorRecord = cp0::theme::kDanger;

constexpr const char *kArecord = "/usr/bin/arecord";
constexpr const char *kFfmpeg = "/usr/bin/ffmpeg";
constexpr const char *kPlayer = "/usr/bin/pw-play";
constexpr const char *kWhisper = "/usr/local/bin/whisper-cli";
constexpr const char *kModel = "/usr/local/share/whisper/ggml-tiny.en-q5_1.bin";
constexpr uint32_t kMaxRecordMs = 600000;
// Shorter clips are almost always an accidental double press.
constexpr uint32_t kMinRecordMs = 700;
constexpr uint32_t kRefreshMs = 100;
constexpr uint32_t kMessageMs = 3000;
constexpr uint32_t kDeleteArmMs = 3000;
constexpr int kListTop = cp0::theme::kBodyTop + 5;
constexpr int kRowH = 18;
constexpr int kBodyBottom = cp0::theme::kFooterRuleY;
constexpr int kTextX = 132;
constexpr std::size_t kRowTextChars = 26;

std::string recordings_dir()
{
    const char *home = std::getenv("HOME");
    return std::string(home && home[0] ? home : "/tmp") + "/Recordings";
}

std::string wav_path(const std::string &stem)
{
    return recordings_dir() + "/" + stem + ".wav";
}

std::string txt_path(const std::string &stem)
{
    return recordings_dir() + "/" + stem + ".txt";
}

std::string work_base(const std::string &stem)
{
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    return std::string(runtime && runtime[0] ? runtime : "/tmp") + "/cp0-rec-" + stem;
}

void remove_work_files(const std::string &stem)
{
    const std::string base = work_base(stem);
    for (const char *suffix : {".wav", ".txt", ".done", ".log"}) unlink((base + suffix).c_str());
}

bool read_file(const std::string &path, std::string &out)
{
    out.clear();
    std::FILE *file = std::fopen(path.c_str(), "r");
    if (!file) return false;
    char buffer[4096];
    std::size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) out.append(buffer, count);
    std::fclose(file);
    return true;
}

bool write_file(const std::string &path, const std::string &text)
{
    std::FILE *file = std::fopen(path.c_str(), "w");
    if (!file) return false;
    const bool ok = std::fputs(text.c_str(), file) >= 0;
    return std::fclose(file) == 0 && ok;
}

uint32_t file_duration_ms(const std::string &path)
{
    struct stat info{};
    if (stat(path.c_str(), &info) != 0) return 0;
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return 0;
    uint8_t header[512];
    const std::size_t count = std::fread(header, 1, sizeof(header), file);
    std::fclose(file);
    return rec::wav_duration_ms(header, count, static_cast<uint64_t>(info.st_size));
}

int wav_tail_level(const std::string &path)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return 0;
    uint8_t pcm[3200];
    std::size_t count = 0;
    // The last 100 ms of 16 kHz mono 16-bit audio.
    if (std::fseek(file, 0, SEEK_END) == 0 && std::ftell(file) > 44 + static_cast<long>(sizeof(pcm)) &&
        std::fseek(file, -static_cast<long>(sizeof(pcm)), SEEK_END) == 0)
        count = std::fread(pcm, 1, sizeof(pcm), file);
    std::fclose(file);
    return rec::peak_level(pcm, count);
}

std::string now_stem()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    return rec::recording_stem(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
                               local.tm_sec);
}

std::string quote(const std::string &path)
{
    return "'" + path + "'";
}

int spawn(const std::string &command)
{
    int pid = -1;
    cp0_signal_process_api({"Spawn", command, "0"}, [&](int code, std::string data) {
        if (code == 0) pid = std::atoi(data.c_str());
    });
    return pid;
}

// Stopping waits for the process group to exit (up to several seconds for a
// busy whisper), so it runs on a short-lived worker; then() runs there too.
void stop_pid(int &pid, std::function<void()> then = {})
{
    if (pid <= 0) {
        if (then) then();
        return;
    }
    const int target = pid;
    pid = -1;
    std::thread([target, then = std::move(then)] {
        try {
            cp0_signal_process_api({"Stop", std::to_string(target)}, [](int, std::string) {});
        } catch (...) {
        }
        if (then) then();
    }).detach();
}

void set_bg_opa(lv_obj_t *obj, lv_opa_t opa)
{
    if (obj && lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) != opa) lv_obj_set_style_bg_opa(obj, opa, LV_PART_MAIN);
}

// Skips redundant label updates; the page re-renders on every timer tick.
void set_text(lv_obj_t *label, const std::string &text)
{
    if (!label) return;
    const char *current = lv_label_get_text(label);
    if (current && text == current) return;
    lv_label_set_text(label, text.c_str());
}

void set_color(lv_obj_t *obj, uint32_t color)
{
    if (!obj) return;
    const lv_color_t value = lv_color_hex(color);
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), value))
        lv_obj_set_style_text_color(obj, value, LV_PART_MAIN);
}

void set_visible(lv_obj_t *obj, bool visible)
{
    if (!obj) return;
    if (visible)
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

bool whisper_installed()
{
    return access(kModel, R_OK) == 0 && access(kWhisper, X_OK) == 0;
}

} // namespace

UIRecPage::UIRecPage() : AppPage()
{
    set_page_title("Rec");
    reclaim_top_bar();
    create_ui();
    load_clips();
    if (!whisper_installed()) show_message("whisper not installed");
    render();
    timer_ = lv_timer_create(timer_cb, kRefreshMs, this);
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

UIRecPage::~UIRecPage()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
    stop_all();
}

lv_obj_t *UIRecPage::make_label(lv_obj_t *parent, int x, int y, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    if (!label) return nullptr;
    lv_obj_set_style_text_font(label, launcher_fonts().get_chrome(11), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    lv_label_set_text(label, "");
    return label;
}

lv_obj_t *UIRecPage::make_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *box = lv_obj_create(parent);
    if (!box) return nullptr;
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_color(box, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    return box;
}

void UIRecPage::create_ui()
{
    if (!ui_APP_Container) return;
    cp0::ui::paint_background(ui_APP_Container);
    const cp0::ui::TitleRow title = cp0::ui::title_row(ui_APP_Container, "rec");
    status_label_ = title.title;
    side_label_ = title.status;

    cp0::ui::panel(ui_APP_Container, 4, cp0::theme::kBodyTop + 2, 312, kBodyBottom - cp0::theme::kBodyTop - 3);
    for (int i = 0; i < kRows; ++i) {
        lv_obj_t *row = make_box(ui_APP_Container, 5, kListTop + i * kRowH, 310, kRowH, kColorSelect);
        if (!row) continue;
        lv_obj_set_style_border_color(row, lv_color_hex(kColorAccent), 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
        rows_[i] = row;
        row_meta_[i] = make_label(row, 6, 3, kColorIdle);
        row_text_[i] = make_label(row, kTextX, 3, kColorIdle);
        if (row_text_[i]) {
            lv_obj_set_width(row_text_[i], 306 - kTextX);
            lv_label_set_long_mode(row_text_[i], LV_LABEL_LONG_CLIP);
        }
    }

    clip_panel_ = lv_obj_create(ui_APP_Container);
    if (clip_panel_) {
        lv_obj_remove_style_all(clip_panel_);
        lv_obj_set_pos(clip_panel_, 6, kListTop);
        lv_obj_set_size(clip_panel_, 308, kBodyBottom - kListTop - 4);
        lv_obj_set_scrollbar_mode(clip_panel_, LV_SCROLLBAR_MODE_OFF);
        clip_text_ = make_label(clip_panel_, 4, 2, kColorText);
        if (clip_text_) {
            lv_obj_set_width(clip_text_, 300);
            lv_label_set_long_mode(clip_text_, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_line_space(clip_text_, 2, 0);
        }
        lv_obj_t *track = make_box(clip_panel_, 4, 40, 300, 4, kColorTrack);
        if (track) clip_progress_ = make_box(track, 0, 0, 0, 4, kColorAccent);
    }

    const cp0::ui::Panel recording =
        cp0::ui::panel(ui_APP_Container, 4, cp0::theme::kBodyTop + 2, 312, kBodyBottom - cp0::theme::kBodyTop - 3);
    cp0::ui::set_brackets(recording.corners, kColorRecord);
    rec_panel_ = recording.box;
    if (rec_panel_) {
        lv_obj_t *badge = make_label(rec_panel_, 0, 14, kColorRecord);
        if (badge) {
            lv_obj_set_width(badge, 312);
            lv_obj_set_style_text_align(badge, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text(badge, "\xE2\x97\x8F recording");
        }
        rec_time_ = make_label(rec_panel_, 0, 32, kColorText);
        if (rec_time_) {
            lv_obj_set_style_text_font(rec_time_, launcher_fonts().get_chrome(22), 0);
            lv_obj_set_width(rec_time_, 312);
            lv_obj_set_style_text_align(rec_time_, LV_TEXT_ALIGN_CENTER, 0);
        }
        lv_obj_t *track = make_box(rec_panel_, 36, 66, 240, 6, kColorTrack);
        if (track) rec_meter_ = make_box(track, 0, 0, 0, 6, kColorAccent);
        rec_note_ = make_label(rec_panel_, 0, 86, kColorDim);
        if (rec_note_) {
            lv_obj_set_width(rec_note_, 312);
            lv_obj_set_style_text_align(rec_note_, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text(rec_note_, "transcribes when you stop");
        }
    }

    footer_ = cp0::ui::footer(ui_APP_Container);
}

void UIRecPage::load_clips()
{
    const std::string keep = selected() ? selected()->stem : std::string();
    // Reuse what was read before unless the wav or transcript changed on disk.
    std::vector<Clip> previous;
    previous.swap(clips_);
    auto cached = [&previous](const std::string &stem) -> Clip * {
        for (Clip &clip : previous)
            if (clip.stem == stem) return &clip;
        return nullptr;
    };
    if (DIR *dir = opendir(recordings_dir().c_str())) {
        while (dirent *entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".wav") != 0) continue;
            const std::string stem = name.substr(0, name.size() - 4);
            if (stem == rec_stem_ && rec_pid_ > 0) continue;
            struct stat wav{};
            struct stat txt{};
            const long long wav_size = stat(wav_path(stem).c_str(), &wav) == 0 ? wav.st_size : -1;
            const bool has_txt = stat(txt_path(stem).c_str(), &txt) == 0;
            Clip clip;
            if (Clip *old = cached(stem); old && old->wav_size == wav_size) {
                clip = std::move(*old);
            } else {
                clip.stem = stem;
                clip.wav_size = wav_size;
                clip.duration_ms = file_duration_ms(wav_path(stem));
            }
            if (!has_txt) {
                clip.has_transcript = false;
                clip.transcript.clear();
                clip.txt_mtime = 0;
            } else if (!clip.has_transcript || clip.txt_mtime != txt.st_mtime) {
                clip.has_transcript = read_file(txt_path(stem), clip.transcript);
                clip.transcript = rec::clean_transcript(clip.transcript);
                clip.txt_mtime = txt.st_mtime;
            }
            clips_.push_back(std::move(clip));
        }
        closedir(dir);
    }
    std::sort(clips_.begin(), clips_.end(), [](const Clip &a, const Clip &b) { return a.stem > b.stem; });
    const int found = find_clip(keep);
    selection_ = found >= 0 ? found : std::clamp(selection_, 0, std::max(0, static_cast<int>(clips_.size()) - 1));
}

int UIRecPage::find_clip(const std::string &stem) const
{
    for (std::size_t i = 0; i < clips_.size(); ++i)
        if (clips_[i].stem == stem) return static_cast<int>(i);
    return -1;
}

const UIRecPage::Clip *UIRecPage::selected() const
{
    if (selection_ < 0 || selection_ >= static_cast<int>(clips_.size())) return nullptr;
    return &clips_[selection_];
}

std::string UIRecPage::clip_status(const Clip &clip, uint32_t &color) const
{
    if (clip.stem == stt_stem_ && stt_pid_ > 0) {
        color = kColorAccent;
        return "transcribing...";
    }
    if (std::find(stt_queue_.begin(), stt_queue_.end(), clip.stem) != stt_queue_.end()) {
        color = kColorDim;
        return "queued";
    }
    if (clip.has_transcript && clip.transcript.empty()) {
        color = kColorDim;
        return "no speech";
    }
    if (clip.has_transcript) return clip.transcript;
    color = kColorDim;
    return "t to transcribe";
}

void UIRecPage::show_message(const std::string &text)
{
    message_ = text;
    message_until_ = lv_tick_get() + kMessageMs;
}

void UIRecPage::set_chips(const std::vector<cp0::ui::Chip> &chips)
{
    if (message_until_ != 0 && lv_tick_get() < message_until_)
        cp0::ui::set_footer_message(footer_, message_.c_str(), kColorText);
    else
        cp0::ui::set_footer(footer_, chips);
}

void UIRecPage::render()
{
    const bool list = view_ == View::List;
    const bool clip = view_ == View::Clip;
    const bool recording = view_ == View::Recording;
    for (lv_obj_t *row : rows_) set_visible(row, list);
    set_visible(clip_panel_, clip);
    set_visible(rec_panel_, recording);
    if (list) render_list();
    if (clip) render_clip();
    if (recording) render_recording();
}

void UIRecPage::render_list()
{
    const uint32_t now = lv_tick_get();
    const int count = static_cast<int>(clips_.size());
    int first = std::max(0, selection_ - kRows / 2);
    if (first + kRows > count) first = std::max(0, count - kRows);
    for (int i = 0; i < kRows; ++i) {
        const int index = first + i;
        const bool has = index < count;
        const bool chosen = has && index == selection_;
        set_bg_opa(rows_[i], chosen ? LV_OPA_COVER : LV_OPA_TRANSP);
        if (rows_[i] && lv_obj_get_style_border_width(rows_[i], LV_PART_MAIN) != (chosen ? 2 : 0))
            lv_obj_set_style_border_width(rows_[i], chosen ? 2 : 0, LV_PART_MAIN);
        if (!has) {
            set_text(row_meta_[i], count == 0 && i == 0 ? "no recordings yet" : "");
            set_text(row_text_[i], "");
            continue;
        }
        const Clip &c = clips_[index];
        char meta[40];
        std::snprintf(meta, sizeof(meta), "%s %5s", rec::stem_label(c.stem).c_str(),
                      rec::format_duration(c.duration_ms).c_str());
        set_text(row_meta_[i], meta);
        set_color(row_meta_[i], chosen ? kColorText : kColorIdle);
        uint32_t color = chosen ? kColorText : kColorIdle;
        const std::string status = clip_status(c, color);
        set_text(row_text_[i], rec::clip_text(status, kRowTextChars));
        set_color(row_text_[i], color);
    }

    std::string status = "recordings";
    uint32_t color = kColorText;
    if (stt_pid_ > 0) {
        status = "stt " + rec::format_duration(now - stt_since_) + " / ~" +
                 rec::format_duration(stt_estimate_ms_);
        if (!stt_queue_.empty()) status += "  +" + std::to_string(stt_queue_.size()) + " queued";
        color = kColorAccent;
    }
    set_text(status_label_, status);
    set_color(status_label_, color);
    set_text(side_label_, std::to_string(count) + (count == 1 ? " clip" : " clips"));
    set_color(side_label_, kColorIdle);

    if (count == 0)
        set_chips({{"spc", "record"}});
    else
        set_chips({{"spc", "record"}, {"ent", "open"}, {"t", "transcribe"}, {"dd", "del"}});
}

void UIRecPage::render_clip()
{
    const Clip *c = selected();
    if (!c) {
        view_ = View::List;
        render();
        return;
    }
    const uint32_t now = lv_tick_get();
    std::string status = rec::stem_label(c->stem) + "  " + rec::format_duration(c->duration_ms);
    uint32_t color = kColorText;
    set_text(status_label_, status);
    set_color(status_label_, color);

    const bool playing = play_pid_ > 0 && play_stem_ == c->stem;
    set_text(side_label_, playing ? "\xE2\x96\xB6 " + rec::format_duration(now - play_since_) : "");
    set_color(side_label_, kColorAccent);

    const bool transcribing = stt_pid_ > 0 && stt_stem_ == c->stem;
    const bool queued = std::find(stt_queue_.begin(), stt_queue_.end(), c->stem) != stt_queue_.end();
    std::string body;
    uint32_t body_color = kColorText;
    if (transcribing) {
        body = "transcribing with whisper\n" + rec::format_duration(now - stt_since_) + " / ~" +
               rec::format_duration(stt_estimate_ms_);
        body_color = kColorAccent;
    } else if (queued) {
        body = "queued for transcription";
        body_color = kColorDim;
    } else if (c->has_transcript && !c->transcript.empty()) {
        body = c->transcript;
    } else if (c->has_transcript) {
        body = "no speech detected\n\npress t to try again";
        body_color = kColorIdle;
    } else {
        body = "no transcript yet\n\npress t to transcribe this recording";
        body_color = kColorIdle;
    }
    set_text(clip_text_, body);
    set_color(clip_text_, body_color);
    if (clip_progress_) {
        lv_obj_t *track = lv_obj_get_parent(clip_progress_);
        set_visible(track, transcribing);
        const uint32_t elapsed = now - stt_since_;
        const uint32_t estimate = std::max<uint32_t>(1, stt_estimate_ms_);
        const int width = static_cast<int>(std::min<uint64_t>(95, uint64_t(elapsed) * 100 / estimate) * 308 / 100);
        lv_obj_set_width(clip_progress_, transcribing ? width : 0);
    }

    set_chips({{"p", playing ? "stop" : "play"},
               {"t", c->has_transcript ? "redo" : "transcribe"},
               {"dd", "delete"},
               {"esc", "back"}});
}

void UIRecPage::render_recording()
{
    const uint32_t elapsed = lv_tick_get() - rec_since_;
    char time_text[16];
    std::snprintf(time_text, sizeof(time_text), "%02u:%02u", elapsed / 60000, (elapsed / 1000) % 60);
    set_text(rec_time_, time_text);
    if (rec_meter_) lv_obj_set_width(rec_meter_, level_ * 240 / 100);
    set_text(status_label_, "rec");
    set_color(status_label_, kColorRecord);
    set_text(side_label_, "max " + rec::format_duration(kMaxRecordMs));
    set_color(side_label_, kColorDim);
    set_chips({{"spc", "stop + transcribe"}, {"esc", "discard"}});
}

void UIRecPage::start_recording()
{
    if (mkdir(recordings_dir().c_str(), 0755) != 0 && errno != EEXIST) {
        show_message("cannot create ~/Recordings");
        return;
    }
    stop_playback();
    rec_stem_ = now_stem();
    rec_pid_ = spawn(std::string("exec ") + kArecord + " -q -f S16_LE -r 16000 -c 1 -d " +
                     std::to_string(kMaxRecordMs / 1000) + " " + quote(wav_path(rec_stem_)));
    if (rec_pid_ <= 0) {
        show_message("cannot open the mic");
        return;
    }
    rec_since_ = lv_tick_get();
    level_ = 0;
    view_ = View::Recording;
}

void UIRecPage::stop_recording(bool keep)
{
    const uint32_t elapsed = lv_tick_get() - rec_since_;
    level_ = 0;
    view_ = View::List;
    const std::string stem = rec_stem_;
    if (!keep || elapsed < kMinRecordMs) {
        const std::string wav = wav_path(stem);
        stop_pid(rec_pid_, [wav] { unlink(wav.c_str()); });
        rec_stem_.clear();
        show_message(keep ? "too short" : "discarded");
        return;
    }
    // The clip is listed and transcribed once arecord has exited and
    // finalised the wav header; on_timer() picks it up from stopped_.
    std::weak_ptr<Stopped> stopped = stopped_;
    stop_pid(rec_pid_, [stopped, stem] {
        if (auto state = stopped.lock()) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->stems.push_back(stem);
        }
    });
    rec_stem_.clear();
    show_message("saving");
}

void UIRecPage::take_stopped_recordings()
{
    std::vector<std::string> stems;
    {
        std::lock_guard<std::mutex> lock(stopped_->mutex);
        stems.swap(stopped_->stems);
    }
    if (stems.empty()) return;
    load_clips();
    for (const std::string &stem : stems) {
        const int index = find_clip(stem);
        if (index >= 0) selection_ = index;
        if (whisper_installed())
            queue_transcription(stem);
        else
            show_message("saved, whisper not installed");
    }
    render();
}

void UIRecPage::queue_transcription(const std::string &stem)
{
    if (!whisper_installed()) {
        show_message("whisper not installed");
        return;
    }
    if ((stem == stt_stem_ && stt_pid_ > 0) ||
        std::find(stt_queue_.begin(), stt_queue_.end(), stem) != stt_queue_.end())
        return;
    stt_queue_.push_back(stem);
    if (stt_pid_ <= 0) start_next_transcription();
}

void UIRecPage::start_next_transcription()
{
    while (stt_pid_ <= 0 && !stt_queue_.empty()) {
        const std::string stem = stt_queue_.front();
        stt_queue_.pop_front();
        if (find_clip(stem) < 0) continue;
        const std::string base = work_base(stem);
        remove_work_files(stem);
        // Convert to the 16 kHz mono PCM whisper reads, levelling quiet speech.
        // nice and three threads keep one core free for the launcher.
        const std::string command =
            std::string("{ ") + kFfmpeg + " -nostdin -v error -y -i " + quote(wav_path(stem)) +
            " -af dynaudnorm -ar 16000 -ac 1 -c:a pcm_s16le " + quote(base + ".wav") + " && nice -n 10 " + kWhisper +
            " -m " + kModel + " -t 3 -l en -nt -np -otxt -of " + quote(base) + " -f " + quote(base + ".wav") +
            "; } >/dev/null 2>" + quote(base + ".log") + "; echo $? > " + quote(base + ".done");
        stt_pid_ = spawn(command);
        if (stt_pid_ <= 0) {
            show_message("cannot start whisper");
            continue;
        }
        stt_stem_ = stem;
        stt_since_ = lv_tick_get();
        const int index = find_clip(stem);
        stt_estimate_ms_ = rec::transcribe_estimate_ms(index >= 0 ? clips_[index].duration_ms : 0);
    }
}

void UIRecPage::finish_transcription()
{
    stop_pid(stt_pid_);
    const std::string stem = stt_stem_;
    stt_stem_.clear();
    const std::string base = work_base(stem);
    std::string done;
    std::string raw;
    read_file(base + ".done", done);
    read_file(base + ".txt", raw);
    remove_work_files(stem);
    if (std::atoi(done.c_str()) != 0) {
        show_message("transcription failed");
    } else {
        const std::string text = rec::clean_transcript(raw);
        if (!write_file(txt_path(stem), text.empty() ? "" : text + "\n")) {
            show_message("cannot save transcript");
        } else {
            const int index = find_clip(stem);
            if (index >= 0) {
                clips_[index].has_transcript = true;
                clips_[index].transcript = text;
            }
            show_message(text.empty() ? "no speech heard" : "transcribed");
        }
    }
    start_next_transcription();
}

void UIRecPage::toggle_playback()
{
    const Clip *c = selected();
    if (!c) return;
    const bool same = play_pid_ > 0 && play_stem_ == c->stem;
    stop_playback();
    if (same) return;
    play_pid_ = spawn(std::string("exec ") + kPlayer + " " + quote(wav_path(c->stem)));
    if (play_pid_ <= 0) {
        show_message("cannot play");
        return;
    }
    play_stem_ = c->stem;
    play_since_ = lv_tick_get();
}

void UIRecPage::stop_playback()
{
    stop_pid(play_pid_);
    play_stem_.clear();
}

void UIRecPage::delete_selected()
{
    const Clip *c = selected();
    if (!c) return;
    const std::string stem = c->stem;
    if (play_stem_ == stem) stop_playback();
    if (stt_stem_ == stem && stt_pid_ > 0) {
        stop_pid(stt_pid_, [stem] { remove_work_files(stem); });
        stt_stem_.clear();
    }
    stt_queue_.erase(std::remove(stt_queue_.begin(), stt_queue_.end(), stem), stt_queue_.end());
    unlink(wav_path(stem).c_str());
    unlink(txt_path(stem).c_str());
    view_ = View::List;
    load_clips();
    show_message("deleted");
    start_next_transcription();
}

void UIRecPage::stop_all()
{
    // A recording in progress is kept: SIGTERM lets arecord finalise the file.
    stop_pid(rec_pid_);
    stop_playback();
    if (stt_pid_ > 0) {
        const std::string stem = stt_stem_;
        stop_pid(stt_pid_, [stem] { remove_work_files(stem); });
    }
    stt_queue_.clear();
}

void UIRecPage::timer_cb(lv_timer_t *timer) noexcept
{
    auto *page = static_cast<UIRecPage *>(lv_timer_get_user_data(timer));
    if (!page) return;
    try {
        page->on_timer();
    } catch (...) {
    }
}

void UIRecPage::on_timer()
{
    take_stopped_recordings();
    const uint32_t now = lv_tick_get();
    bool changed = false;
    if (view_ == View::Recording) {
        if (now - rec_since_ >= kMaxRecordMs) {
            stop_recording(true);
        } else {
            level_ = wav_tail_level(wav_path(rec_stem_));
        }
    }
    if (stt_pid_ > 0 && access((work_base(stt_stem_) + ".done").c_str(), F_OK) == 0) finish_transcription();
    if (play_pid_ > 0) {
        const int index = find_clip(play_stem_);
        const uint32_t length = index >= 0 ? clips_[index].duration_ms : 0;
        if (now - play_since_ > length + 500) stop_playback();
    }
    if (message_until_ != 0 && now >= message_until_) {
        message_until_ = 0;
        changed = true;
    }
    // Idle pages do not redraw; only live recording, transcription or
    // playback progress needs the periodic refresh.
    if (!changed && view_ != View::Recording && stt_pid_ <= 0 && play_pid_ <= 0) return;
    render();
}

void UIRecPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UIRecPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item) return;
    const uint32_t key = item->semantic_key ? item->semantic_key : item->key_code;
    try {
        page->handle_key(key, item->key_state == KBD_KEY_REPEATED);
    } catch (...) {
    }
}

void UIRecPage::handle_key(uint32_t key, bool repeat)
{
    if (repeat && key != KEY_UP && key != KEY_DOWN) return;
    const bool enter = key == KEY_ENTER || key == KEY_KPENTER;
    if (key != KEY_D) delete_armed_until_ = 0;

    if (view_ == View::Recording) {
        if (enter || key == KEY_SPACE || key == KEY_R) stop_recording(true);
        if (key == KEY_ESC) stop_recording(false);
        render();
        return;
    }

    if (key == KEY_D) {
        if (!selected()) return;
        if (lv_tick_get() < delete_armed_until_) {
            delete_armed_until_ = 0;
            delete_selected();
        } else {
            delete_armed_until_ = lv_tick_get() + kDeleteArmMs;
            show_message("press d again to delete");
        }
        render();
        return;
    }
    if (key == KEY_T) {
        if (const Clip *c = selected()) queue_transcription(c->stem);
        render();
        return;
    }

    if (view_ == View::Clip) {
        if (key == KEY_ESC || key == KEY_LEFT) {
            stop_playback();
            view_ = View::List;
        } else if (enter || key == KEY_P) {
            toggle_playback();
        } else if (clip_panel_ && (key == KEY_UP || key == KEY_DOWN)) {
            lv_obj_scroll_by_bounded(clip_panel_, 0, key == KEY_UP ? 3 * kRowH : -3 * kRowH, LV_ANIM_OFF);
        }
        render();
        return;
    }

    switch (key) {
    case KEY_ESC:
        if (navigate_home) navigate_home();
        return;
    case KEY_UP:
        if (selection_ > 0) --selection_;
        break;
    case KEY_DOWN:
        if (selection_ + 1 < static_cast<int>(clips_.size())) ++selection_;
        break;
    case KEY_SPACE:
    case KEY_R:
        start_recording();
        break;
    case KEY_RIGHT:
        if (selected()) {
            view_ = View::Clip;
            if (clip_panel_) lv_obj_scroll_to_y(clip_panel_, 0, LV_ANIM_OFF);
        }
        break;
    default:
        if (enter && selected()) {
            view_ = View::Clip;
            if (clip_panel_) lv_obj_scroll_to_y(clip_panel_, 0, LV_ANIM_OFF);
        }
        break;
    }
    render();
}

/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_audio.hpp"

#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "input_keys.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace theme = cp0::theme;

constexpr uint32_t kRefreshMs = 33;
constexpr uint32_t kMessageMs = 2500;
constexpr uint32_t kNoDataMs = 1500;
constexpr float kBarFloorDb = -80.0f;
constexpr float kBarDecayDb = 1.5f;
constexpr float kHoldDecayDb = 0.25f;
constexpr float kClipDb = -1.0f;
constexpr int kMinGate = -90;
constexpr int kMaxGate = -20;

uint16_t rgb565(uint32_t rgb)
{
    return lv_color_to_u16(lv_color_hex(rgb));
}

int spawn(const std::string &command)
{
    int pid = -1;
    cp0_signal_process_api({"Spawn", command, "0"}, [&](int code, std::string data) {
        if (code == 0) pid = std::atoi(data.c_str());
    });
    return pid;
}

std::string runtime_dir()
{
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    return runtime && runtime[0] ? runtime : "/tmp";
}

void set_text(lv_obj_t *label, const std::string &text, uint32_t color)
{
    cp0::ui::set_text(label, text.c_str(), color);
}

int bar_height(float db, int height)
{
    const float t = std::clamp((db - kBarFloorDb) / -kBarFloorDb, 0.0f, 1.0f);
    return static_cast<int>(std::lround(t * static_cast<float>(height)));
}

} // namespace

UIAudioPage::UIAudioPage() : AppPage()
{
    set_page_title("Audio");
    reclaim_top_bar();
    bars_.fill(kBarFloorDb);
    hold_.fill(kBarFloorDb);
    pixels_.assign(static_cast<std::size_t>(kWidth) * kCanvasH, rgb565(theme::kPanel));
    create_ui();
    mic_ok_ = start_capture();
    if (!mic_ok_) show_message("cannot open the mic");
    last_data_ms_ = lv_tick_get();
    render();
    draw();
    timer_ = lv_timer_create(timer_cb, kRefreshMs, this);
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

UIAudioPage::~UIAudioPage()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
    stop_capture();
}

void UIAudioPage::create_ui()
{
    if (!ui_APP_Container) return;
    cp0::ui::paint_background(ui_APP_Container);
    const cp0::ui::TitleRow title = cp0::ui::title_row(ui_APP_Container, "audio");
    status_ = title.status;

    freq_ = cp0::ui::readout(ui_APP_Container, 4, 19, 178, 40, "freq");
    note_ = cp0::ui::readout(ui_APP_Container, 186, 19, 130, 40, "note");
    cp0::ui::panel(ui_APP_Container, 4, 63, 312, 76);

    canvas_ = lv_canvas_create(ui_APP_Container);
    if (canvas_) {
        lv_canvas_set_buffer(canvas_, pixels_.data(), kWidth, kCanvasH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(canvas_, kCanvasX, kCanvasTop);
    }
    gate_label_ = cp0::ui::label(ui_APP_Container, kCanvasX + kWidth - 4 - 70, kCanvasTop, theme::kDim);
    if (gate_label_) {
        lv_obj_set_width(gate_label_, 70);
        lv_obj_set_style_text_align(gate_label_, LV_TEXT_ALIGN_RIGHT, 0);
    }

    static constexpr struct { float hz; const char *text; } kTicks[] = {
        {50.0f, "50"}, {100.0f, "100"}, {200.0f, "200"}, {500.0f, "500"},
        {1000.0f, "1k"}, {2000.0f, "2k"}, {5000.0f, "5k"}, {10000.0f, "10k"},
    };
    for (const auto &tick : kTicks) {
        lv_obj_t *label = cp0::ui::label(ui_APP_Container, 0, kTickY, theme::kDim);
        if (!label) continue;
        lv_label_set_text(label, tick.text);
        lv_obj_set_width(label, 30);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        const int x = kCanvasX + static_cast<int>(audio::log_position(tick.hz, audio::kMinHz, audio::kMaxHz) * kWidth);
        lv_obj_set_x(label, std::clamp(x - 15, 0, theme::kScreenW - 30));
    }

    footer_ = cp0::ui::footer(ui_APP_Container);
}

bool UIAudioPage::start_capture()
{
    fifo_path_ = runtime_dir() + "/cp0-audio-" + std::to_string(getpid()) + ".pcm";
    unlink(fifo_path_.c_str());
    if (mkfifo(fifo_path_.c_str(), 0600) != 0) return false;
    // The read end is opened first and non-blocking, so the shell's open for
    // writing does not wait and the UI thread never blocks on a read.
    fifo_fd_ = open(fifo_path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fifo_fd_ < 0) {
        unlink(fifo_path_.c_str());
        return false;
    }
    rec_pid_ = spawn("exec /usr/bin/arecord -q -f S16_LE -r " + std::to_string(audio::kSampleRate) +
                     " -c 1 -t raw > '" + fifo_path_ + "'");
    return rec_pid_ > 0;
}

void UIAudioPage::stop_capture()
{
    if (rec_pid_ > 0) {
        try {
            cp0_signal_process_api({"Stop", std::to_string(rec_pid_)}, [](int, std::string) {});
        } catch (...) {
        }
    }
    rec_pid_ = -1;
    if (fifo_fd_ >= 0) close(fifo_fd_);
    fifo_fd_ = -1;
    if (!fifo_path_.empty()) unlink(fifo_path_.c_str());
}

void UIAudioPage::read_capture()
{
    if (fifo_fd_ < 0) return;
    uint8_t buffer[16384];
    while (true) {
        const ssize_t count = read(fifo_fd_, buffer, sizeof(buffer));
        if (count <= 0) break;
        last_data_ms_ = lv_tick_get();
        if (frozen_) continue;
        std::size_t index = 0;
        if (odd_byte_valid_) {
            samples_.push_back(static_cast<int16_t>(odd_byte_ | (buffer[0] << 8)));
            odd_byte_valid_ = false;
            index = 1;
        }
        for (; index + 1 < static_cast<std::size_t>(count); index += 2)
            samples_.push_back(static_cast<int16_t>(buffer[index] | (buffer[index + 1] << 8)));
        if (index < static_cast<std::size_t>(count)) {
            odd_byte_ = buffer[index];
            odd_byte_valid_ = true;
        }
    }
    const std::size_t keep = static_cast<std::size_t>(audio::kFftSize);
    if (samples_.size() > keep) samples_.erase(samples_.begin(), samples_.end() - keep);
}

void UIAudioPage::analyse()
{
    if (frozen_ || samples_.size() < static_cast<std::size_t>(audio::kFftSize)) return;
    const std::vector<float> db = audio::spectrum_db(samples_.data(), samples_.size());
    peak_ = audio::find_peak(db, audio::kSampleRate, audio::kMinHz, audio::kMaxHz, static_cast<float>(gate_db_));
    if (peak_.valid) last_hz_ = peak_.hz;
    level_db_ = audio::rms_dbfs(samples_.data(), samples_.size());
    const std::vector<float> bands = audio::log_bands(db, audio::kSampleRate, kBands, audio::kMinHz, audio::kMaxHz);
    for (int i = 0; i < kBands; ++i) {
        bars_[i] = std::max(bands[i], bars_[i] - kBarDecayDb);
        hold_[i] = std::max(bands[i], hold_[i] - kHoldDecayDb);
    }
}

void UIAudioPage::draw()
{
    if (!canvas_) return;
    const uint16_t black = rgb565(theme::kPanel);
    const uint16_t bar = rgb565(theme::kIdle);
    const uint16_t hot = rgb565(theme::kAccent);
    const uint16_t clip = rgb565(theme::kDanger);
    const uint16_t hold = rgb565(theme::kText);
    const uint16_t gate = rgb565(theme::kWarn);
    const uint16_t base = rgb565(theme::kLine);
    std::fill(pixels_.begin(), pixels_.end(), black);

    const int peak_band = peak_.valid
        ? std::clamp(static_cast<int>(audio::log_position(peak_.hz, audio::kMinHz, audio::kMaxHz) * kBands), 0, kBands - 1)
        : -1;
    const auto put = [&](int x, int y, uint16_t color) {
        if (x >= 0 && x < kWidth && y >= 0 && y < kCanvasH) pixels_[static_cast<std::size_t>(y) * kWidth + x] = color;
    };

    for (int x = 0; x < kWidth; ++x) put(x, kCanvasH - 1, base);
    const int gate_y = kCanvasH - 1 - bar_height(static_cast<float>(gate_db_), kCanvasH - 1);
    for (int x = 0; x < kWidth; x += 3) put(x, gate_y, gate);
    if (gate_label_) lv_obj_set_y(gate_label_, kCanvasTop + std::max(0, gate_y - 13));

    for (int band = 0; band < kBands; ++band) {
        const int x0 = band * kBandW;
        const int h = bar_height(bars_[band], kCanvasH - 1);
        const uint16_t color = bars_[band] >= kClipDb ? clip : band == peak_band ? hot : bar;
        for (int y = kCanvasH - 1 - h; y < kCanvasH - 1; ++y) {
            if ((kCanvasH - 1 - y) % 3 == 0) continue;
            for (int x = x0; x < x0 + kBandW - 1; ++x) put(x, y, color);
        }
        const int hold_y = kCanvasH - 2 - bar_height(hold_[band], kCanvasH - 1);
        if (hold_[band] > kBarFloorDb + 1.0f)
            for (int x = x0; x < x0 + kBandW - 1; ++x) put(x, std::max(0, hold_y), hold);
    }
    lv_obj_invalidate(canvas_);
}

void UIAudioPage::render()
{
    const uint32_t now = lv_tick_get();
    const bool live = mic_ok_ && now - last_data_ms_ < kNoDataMs;

    if (!mic_ok_)
        set_text(status_, "no mic", theme::kDanger);
    else if (frozen_)
        set_text(status_, "frozen", theme::kWarn);
    else if (!live)
        set_text(status_, "no signal", theme::kWarn);
    else
        set_text(status_, std::to_string(audio::kSampleRate / 1000) + " kHz", theme::kIdle);

    if (last_hz_ > 0.0f) {
        const std::string text = audio::format_hz(last_hz_);
        const std::size_t space = text.find(' ');
        cp0::ui::set_readout(freq_, text.substr(0, space), space == std::string::npos ? "" : text.substr(space + 1),
                             peak_.valid ? theme::kText : theme::kDim);
        const audio::Note note = audio::nearest_note(last_hz_);
        cp0::ui::set_readout(note_, note.name, (note.cents > 0 ? "+" : "") + std::to_string(note.cents) + "c",
                             peak_.valid ? theme::kAccent : theme::kDim);
    } else {
        cp0::ui::set_readout(freq_, "--", "Hz", theme::kDim);
        cp0::ui::set_readout(note_, "--", "", theme::kDim);
    }
    if (gate_label_) set_text(gate_label_, "gate " + std::to_string(gate_db_), theme::kDim);
    if (mic_ok_ && !frozen_ && live)
        set_text(status_, std::to_string(static_cast<int>(std::lround(level_db_))) + " dBFS", theme::kIdle);

    if (now < message_until_)
        cp0::ui::set_footer_message(footer_, message_.c_str(), mic_ok_ ? theme::kAccent : theme::kDanger);
    else
        cp0::ui::set_footer(footer_, {{"spc", frozen_ ? "resume" : "freeze"}, {"up/dn", "gate"}, {"r", "reset"}});
}

void UIAudioPage::reset_hold()
{
    bars_.fill(kBarFloorDb);
    hold_.fill(kBarFloorDb);
    last_hz_ = 0.0f;
    peak_ = {};
}

void UIAudioPage::show_message(const std::string &text)
{
    message_ = text;
    message_until_ = lv_tick_get() + kMessageMs;
}

void UIAudioPage::on_timer()
{
    read_capture();
    analyse();
    if (!frozen_) draw();
    render();
}

void UIAudioPage::timer_cb(lv_timer_t *timer) noexcept
{
    auto *page = static_cast<UIAudioPage *>(lv_timer_get_user_data(timer));
    if (!page) return;
    try {
        page->on_timer();
    } catch (...) {
    }
}

void UIAudioPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UIAudioPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item) return;
    const bool repeat = item->key_state == KBD_KEY_REPEATED;
    const uint32_t key = item->semantic_key ? item->semantic_key : item->key_code;
    if (repeat && key != KEY_UP && key != KEY_DOWN) return;
    try {
        page->handle_key(key);
    } catch (...) {
    }
}

void UIAudioPage::handle_key(uint32_t key)
{
    switch (key) {
    case KEY_ESC:
        if (navigate_home) navigate_home();
        return;
    case KEY_SPACE:
        frozen_ = !frozen_;
        break;
    case KEY_UP:
        gate_db_ = std::min(kMaxGate, gate_db_ + 5);
        break;
    case KEY_DOWN:
        gate_db_ = std::max(kMinGate, gate_db_ - 5);
        break;
    case KEY_R:
        reset_hold();
        show_message("peak hold cleared");
        break;
    default:
        return;
    }
    draw();
    render();
}

/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_fox.hpp"

#include "../app_support.hpp"
#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "input_keys.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

static const uint32_t &kColorAccent = cp0::theme::kAccent;
static const uint32_t &kColorText = cp0::theme::kText;
static const uint32_t &kColorIdle = cp0::theme::kIdle;
static const uint32_t &kColorDim = cp0::theme::kDim;
static const uint32_t &kColorWarn = cp0::theme::kWarn;
static const uint32_t &kColorRed = cp0::theme::kDanger;
// The trace fill the SDR scope uses under its line.
static const uint32_t &kColorFill = cp0::theme::kFill;
constexpr uint32_t kRefreshMs = 50;
// The big reading turns accent above this level.
constexpr float kStrongDbm = -62.0f;
// Audio is written this far ahead of real time so the player never runs dry
// between two pumps of the worker loop.
constexpr int kAudioLeadMs = 40;

constexpr const char *kFreqConfigKey = "fox_freq";
constexpr const char *kBeepConfigKey = "fox_beep";

int config_int(const char *key, int fallback)
{
    int value = fallback;
    cp0_signal_config_api({"GetInt", key, std::to_string(fallback)}, [&](int code, std::string data) {
        if (code == 0) value = std::atoi(data.c_str());
    });
    return value;
}

void set_config_int(const char *key, int value)
{
    cp0_signal_config_api({"SetInt", key, std::to_string(value)}, nullptr);
    cp0_signal_config_api({"Save"}, nullptr);
}

void set_visible(lv_obj_t *obj, bool on)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == !on) return;
    if (on)
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

std::string signed_db(float value)
{
    const int rounded = static_cast<int>(std::lround(value));
    return (rounded > 0 ? "+" : "") + std::to_string(rounded);
}

} // namespace

UIFoxPage::UIFoxPage() : AppPage()
{
    set_page_title("Fox");
    reclaim_top_bar();
    const int saved = config_int(kFreqConfigKey, static_cast<int>(fox::default_frequency_index()));
    freq_index_ = saved >= 0 && static_cast<std::size_t>(saved) < fox::frequency_count()
                      ? static_cast<std::size_t>(saved)
                      : fox::default_frequency_index();
    freq_hz_.store(fox::frequency_hz(freq_index_));
    pixels_.assign(static_cast<std::size_t>(kStripW) * kStripH, app_support::rgb565(cp0::theme::kPanel));
    create_ui();

    audio_path_ = app_support::runtime_dir() + "/cp0-fox-audio-" + std::to_string(getpid());
    unlink(audio_path_.c_str());
    beep_available_ = app_support::tool_present("pacat") && mkfifo(audio_path_.c_str(), 0600) == 0;
    if (beep_available_ && config_int(kBeepConfigKey, 1) != 0) set_beep(true);

    render();
    worker_ = std::thread([this] { worker_loop(); });
    timer_ = lv_timer_create(timer_cb, kRefreshMs, this);
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

UIFoxPage::~UIFoxPage()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
    stop_ = true;
    if (worker_.joinable()) worker_.join();
    radio_.close();
    app_support::stop(audio_pid_);
    unlink(audio_path_.c_str());
}

void UIFoxPage::create_ui()
{
    if (!ui_APP_Container) return;
    cp0::ui::paint_background(ui_APP_Container);
    const cp0::ui::TitleRow row = cp0::ui::title_row(ui_APP_Container, "fox");
    status_ = row.status;

    rssi_ = cp0::ui::readout(ui_APP_Container, 4, 19, 154, 40, "rssi  dBm");
    peak_ = cp0::ui::readout(ui_APP_Container, 162, 19, 154, 40, "peak  dBm");
    if (peak_.panel.box) {
        trend_ = cp0::ui::label(peak_.panel.box, 154 - 5 - 60, 2, kColorDim);
        if (trend_) {
            lv_obj_set_width(trend_, 60);
            lv_obj_set_style_text_align(trend_, LV_TEXT_ALIGN_RIGHT, 0);
        }
    }

    cp0::ui::panel(ui_APP_Container, 4, 63, 312, 24);
    bar_ = cp0::ui::segbar(ui_APP_Container, kBarX, 68, kBarW, 14);
    zero_mark_ = cp0::ui::box(ui_APP_Container, kBarX, 66, 1, 18, kColorWarn);
    peak_mark_ = cp0::ui::box(ui_APP_Container, kBarX, 66, 1, 18, kColorText);

    cp0::ui::panel(ui_APP_Container, 4, 91, 312, 60);
    canvas_ = lv_canvas_create(ui_APP_Container);
    if (canvas_) {
        lv_canvas_set_buffer(canvas_, pixels_.data(), kStripW, kStripH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(canvas_, kStripX, kStripY);
    }
    span_ = cp0::ui::label(ui_APP_Container, 9, 93, kColorDim);
    if (span_)
        lv_label_set_text(span_,
                          (std::to_string(history_.seconds(static_cast<float>(kReadsPerSecond))) + " s").c_str());
    big_ = cp0::ui::label(ui_APP_Container, 12, 98, kColorText, true);
    sub_ = cp0::ui::label(ui_APP_Container, 12, 130, kColorIdle);
    if (sub_) {
        lv_obj_set_width(sub_, 296);
        lv_label_set_long_mode(sub_, LV_LABEL_LONG_CLIP);
    }

    hint_ = cp0::ui::footer(ui_APP_Container);
}

void UIFoxPage::worker_loop()
{
    // SIGPIPE from a write is delivered to the writing thread; blocked here,
    // a dead pacat turns into EPIPE instead of killing the launcher.
    sigset_t sigpipe;
    sigemptyset(&sigpipe);
    sigaddset(&sigpipe, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &sigpipe, nullptr);

    std::string error;
    if (!radio_.open(error) || !radio_.begin_finder()) {
        radio_.close();
        std::lock_guard<std::mutex> lock(mutex_);
        radio_error_ = error.empty() ? "CC1101 setup failed" : error;
        radio_state_ = static_cast<int>(Radio::Failed);
        return;
    }
    radio_state_ = static_cast<int>(Radio::Ready);

    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::microseconds(1000000 / kReadsPerSecond);
    fox::Beeper beeper;
    std::vector<int16_t> samples;
    int audio_fd = -1;
    clock::time_point audio_start;
    int64_t audio_written = 0;

    // Keeps a continuous stream going to pacat, silence included, so a beep
    // is never held back waiting for the player's buffer to fill.
    auto pump_audio = [&] {
        if (!beep_on_) {
            if (audio_fd >= 0) close(audio_fd);
            audio_fd = -1;
            return;
        }
        if (audio_fd < 0) {
            // Fails until pacat has the pipe open for reading; retried.
            audio_fd = open(audio_path_.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
            if (audio_fd < 0) return;
            audio_start = clock::now();
            audio_written = 0;
        }
        const int64_t elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - audio_start).count();
        int64_t due = (elapsed_ms + kAudioLeadMs) * fox::Beeper::kRate / 1000 - audio_written;
        if (due <= 0) return;
        if (due > fox::Beeper::kRate / 2) {
            // Far behind (the thread was held up): skip ahead instead of
            // playing a backlog late.
            audio_written += due - fox::Beeper::kRate / 10;
            due = fox::Beeper::kRate / 10;
        }
        samples.resize(static_cast<std::size_t>(due));
        beeper.render(samples.data(), samples.size(), beep_interval_ms_);
        audio_written += due;
        if (write(audio_fd, samples.data(), samples.size() * sizeof(int16_t)) < 0 && errno == EPIPE) {
            close(audio_fd);
            audio_fd = -1;
        }
    };

    clock::time_point next = clock::now();
    while (!stop_) {
        const uint32_t hz = freq_hz_;
        const int dbm = radio_.finder_read(hz);
        {
            // A reading taken on the previous frequency is dropped.
            std::lock_guard<std::mutex> lock(mutex_);
            if (freq_hz_ == hz) pending_.push_back(dbm);
        }
        next += period;
        if (clock::now() > next + 4 * period) next = clock::now();
        do {
            pump_audio();
            usleep(5000);
        } while (!stop_ && clock::now() < next);
    }
    if (audio_fd >= 0) close(audio_fd);
    radio_.close();
}

void UIFoxPage::set_frequency(std::size_t index)
{
    freq_index_ = index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        freq_hz_ = fox::frequency_hz(index);
        pending_.clear();
    }
    tracker_.reset();
    history_.clear();
    beep_interval_ms_ = 0;
    set_config_int(kFreqConfigKey, static_cast<int>(index));
    render();
}

void UIFoxPage::set_beep(bool on)
{
    if (!beep_available_ || on == beep_on_.load()) return;
    if (on) {
        // --latency-msec is unverified against the pacat on the device; if it
        // is rejected pacat exits and the page stays silent.
        audio_pid_ = app_support::spawn("exec pacat --raw --rate=" + std::to_string(fox::Beeper::kRate) +
                                        " --channels=1 --format=s16le --latency-msec=60 < '" + audio_path_ + "'");
        if (audio_pid_ <= 0) return;
        beep_on_ = true;
    } else {
        beep_on_ = false;
        app_support::stop(audio_pid_);
    }
}

void UIFoxPage::draw_strip()
{
    if (!canvas_) return;
    const uint16_t bg = app_support::rgb565(cp0::theme::kPanel);
    const uint16_t line = app_support::rgb565(kColorAccent);
    const uint16_t fill = app_support::rgb565(kColorFill);
    auto y_for = [](float dbm) {
        return (kStripH - 1) - static_cast<int>(std::lround(fox::bar_fraction(dbm) * static_cast<float>(kStripH - 2)));
    };
    std::fill(pixels_.begin(), pixels_.end(), bg);
    const std::vector<float> &columns = history_.columns();
    const int count = std::min(static_cast<int>(columns.size()), kStripW);
    for (int i = 0; i < count; ++i) {
        // Newest column at the right edge.
        const int x = kStripW - count + i;
        const int y = y_for(columns[columns.size() - static_cast<std::size_t>(count) + static_cast<std::size_t>(i)]);
        pixels_[static_cast<std::size_t>(y) * kStripW + x] = line;
        for (int below = y + 1; below < kStripH; ++below) pixels_[static_cast<std::size_t>(below) * kStripW + x] = fill;
    }
    if (tracker_.valid()) {
        const uint16_t peak = app_support::rgb565(kColorDim);
        const int y = y_for(tracker_.peak());
        for (int x = 0; x < kStripW; x += 2) pixels_[static_cast<std::size_t>(y) * kStripW + x] = peak;
    }
    if (tracker_.zeroed()) {
        const uint16_t zero = app_support::rgb565(kColorWarn);
        const int y = y_for(tracker_.zero());
        for (int x = 1; x < kStripW; x += 4) pixels_[static_cast<std::size_t>(y) * kStripW + x] = zero;
    }
    lv_obj_invalidate(canvas_);
}

void UIFoxPage::render()
{
    const Radio state = static_cast<Radio>(radio_state_.load());
    const bool ready = state == Radio::Ready;
    const bool valid = ready && tracker_.valid();
    const std::string mhz = fox::format_mhz(fox::frequency_hz(freq_index_));

    if (state == Radio::Failed)
        cp0::ui::set_text(status_, "no radio", kColorRed);
    else if (!ready)
        cp0::ui::set_text(status_, (mhz + "  opening").c_str(), kColorIdle);
    else
        cp0::ui::set_text(status_, (mhz + "  hunting").c_str(), kColorAccent);

    const float level = tracker_.level();
    if (!valid) {
        if (rssi_.label) cp0::ui::set_text(rssi_.label, "rssi  dBm", kColorDim);
        cp0::ui::set_readout(rssi_, "--", "", kColorIdle);
        cp0::ui::set_readout(peak_, "--", "", kColorIdle);
        cp0::ui::set_text(trend_, "", kColorDim);
    } else {
        if (tracker_.zeroed()) {
            const float delta = tracker_.delta();
            const std::string caption =
                "delta  dB   ref " + std::to_string(static_cast<int>(std::lround(tracker_.zero())));
            if (rssi_.label) cp0::ui::set_text(rssi_.label, caption.c_str(), kColorDim);
            cp0::ui::set_readout(rssi_, signed_db(delta), "",
                                 delta >= 1.0f ? kColorAccent : delta <= -1.0f ? kColorRed : kColorText);
        } else {
            if (rssi_.label) cp0::ui::set_text(rssi_.label, "rssi  dBm", kColorDim);
            cp0::ui::set_readout(rssi_, std::to_string(static_cast<int>(std::lround(level))), "",
                                 level > kStrongDbm ? kColorAccent : kColorText);
        }
        cp0::ui::set_readout(peak_, std::to_string(static_cast<int>(std::lround(tracker_.peak()))), "", kColorIdle);
        const fox::Trend trend = tracker_.trend();
        cp0::ui::set_text(trend_, fox::trend_label(trend),
                          trend == fox::Trend::Warmer ? kColorAccent
                          : trend == fox::Trend::Colder ? kColorRed
                                                        : kColorDim);
    }

    cp0::ui::set_segbar(bar_, valid ? fox::bar_fraction(level) : 0.0f, kColorAccent);
    auto place = [](lv_obj_t *mark, float dbm) {
        if (!mark) return;
        const int x = kBarX + static_cast<int>(std::lround(fox::bar_fraction(dbm) * static_cast<float>(kBarW - 1)));
        if (lv_obj_get_x(mark) != x) lv_obj_set_x(mark, x);
    };
    set_visible(peak_mark_, valid);
    if (valid) place(peak_mark_, tracker_.peak());
    set_visible(zero_mark_, valid && tracker_.zeroed());
    if (valid && tracker_.zeroed()) place(zero_mark_, tracker_.zero());

    const bool failed = state == Radio::Failed;
    set_visible(canvas_, !failed);
    set_visible(span_, !failed);
    set_visible(big_, failed);
    set_visible(sub_, failed);
    if (failed) {
        std::string error;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            error = radio_error_;
        }
        if (big_) cp0::ui::set_text(big_, "no radio", kColorText);
        if (sub_) cp0::ui::set_text(sub_, cp0::ui::lower(error).c_str(), kColorIdle);
    }

    std::vector<cp0::ui::Chip> chips = {{"< >", "freq"}};
    if (beep_available_) chips.push_back({"b", beep_on_ ? "mute" : "beep"});
    chips.push_back({"h", "reset peak"});
    chips.push_back({"z", tracker_.zeroed() ? "clear" : "zero"});
    cp0::ui::set_footer(hint_, chips);
}

void UIFoxPage::timer_cb(lv_timer_t *timer) noexcept
{
    auto *page = static_cast<UIFoxPage *>(lv_timer_get_user_data(timer));
    if (!page) return;
    try {
        page->on_timer();
    } catch (...) {
    }
}

void UIFoxPage::on_timer()
{
    std::vector<int> readings;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        readings.swap(pending_);
    }
    for (int dbm : readings) {
        tracker_.add(dbm);
        history_.push(tracker_.level());
    }
    const Radio state = static_cast<Radio>(radio_state_.load());
    if (readings.empty() && state == shown_state_) return;
    shown_state_ = state;
    // Without a radio there is nothing to beep at; the saved setting stays.
    if (state == Radio::Failed) set_beep(false);
    beep_interval_ms_ = tracker_.valid() ? fox::beep_interval_ms(tracker_.level()) : 0;
    render();
    if (!readings.empty()) draw_strip();
}

void UIFoxPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UIFoxPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item) return;
    // Z is the zero key here, so its navigation alias (left) is not used.
    const uint32_t semantic = item->semantic_key ? item->semantic_key : item->key_code;
    const uint32_t key = item->key_code == KEY_Z ? static_cast<uint32_t>(KEY_Z) : semantic;
    try {
        page->handle_key(key, item->key_state == KBD_KEY_REPEATED);
    } catch (...) {
    }
}

void UIFoxPage::handle_key(uint32_t key, bool repeat)
{
    if (repeat) return;
    switch (key) {
    case KEY_ESC:
        if (navigate_home) navigate_home();
        return;
    case KEY_LEFT:
    case KEY_RIGHT:
        set_frequency(fox::step_frequency(freq_index_, key == KEY_RIGHT ? 1 : -1));
        draw_strip();
        return;
    case KEY_H:
        tracker_.reset_peak();
        break;
    case KEY_Z:
        if (!tracker_.valid()) return;
        if (tracker_.zeroed())
            tracker_.clear_zero();
        else
            tracker_.set_zero();
        break;
    case KEY_B:
        if (!beep_available_) return;
        set_beep(!beep_on_.load());
        set_config_int(kBeepConfigKey, beep_on_ ? 1 : 0);
        break;
    default:
        return;
    }
    render();
    draw_strip();
}

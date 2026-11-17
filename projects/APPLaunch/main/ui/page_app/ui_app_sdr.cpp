/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_sdr.hpp"

#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "input_keys.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cmath>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

namespace {

constexpr uint32_t kColorAccent = cp0::theme::kAccent;
constexpr uint32_t kColorText = cp0::theme::kText;
constexpr uint32_t kColorIdle = cp0::theme::kIdle;
constexpr uint32_t kColorDim = cp0::theme::kDim;
constexpr uint32_t kColorGrid = cp0::theme::kLine;
constexpr uint32_t kColorFill = 0x0D2216;
constexpr uint32_t kColorRec = cp0::theme::kDanger;
constexpr uint32_t kRefreshMs = 66;
constexpr uint32_t kMessageMs = 3000;
constexpr uint32_t kDetectEveryMs = 3000;
// The scope scale follows the noise floor: 6 dB below it to 54 dB above.
constexpr float kScopeBelowFloor = 6.0f;
constexpr float kScopeRange = 60.0f;
constexpr float kScopeSmoothing = 0.5f;
constexpr int kMaxDevices = 8;

constexpr const char *kAudioOutConfigKey = "sdr_audio_out";

bool config_audio_out()
{
    int value = 1;
    cp0_signal_config_api({"GetInt", kAudioOutConfigKey, "1"}, [&](int code, std::string data) {
        if (code == 0) value = std::atoi(data.c_str());
    });
    return value != 0;
}

void set_config_audio_out(bool on)
{
    cp0_signal_config_api({"SetInt", kAudioOutConfigKey, on ? "1" : "0"}, nullptr);
    cp0_signal_config_api({"Save"}, nullptr);
}

uint16_t rgb565(uint32_t rgb)
{
    return lv_color_to_u16(lv_color_hex(rgb));
}

std::string runtime_dir()
{
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    return runtime && runtime[0] ? runtime : "/tmp";
}

std::string home_dir()
{
    const char *home = std::getenv("HOME");
    return home && home[0] ? home : "/tmp";
}

int spawn(const std::string &command)
{
    int pid = -1;
    cp0_signal_process_api({"Spawn", command, "0"}, [&](int code, std::string data) {
        if (code == 0) pid = std::atoi(data.c_str());
    });
    return pid;
}

bool tool_present()
{
    return access("/usr/bin/rtl_test", X_OK) == 0 || access("/usr/local/bin/rtl_test", X_OK) == 0;
}

} // namespace

UISdrPage::UISdrPage() : AppPage()
{
    set_page_title("SDR");
    reclaim_top_bar();
    audio_out_ = config_audio_out();
    detect_path_ = runtime_dir() + "/cp0-sdr-detect";
    out_path_ = runtime_dir() + "/cp0-sdr-out";
    iq_path_ = runtime_dir() + "/cp0-sdr-iq-" + std::to_string(getpid());
    audio_path_ = runtime_dir() + "/cp0-sdr-audio-" + std::to_string(getpid());
    for (int i = 0; i < 64; ++i)
        palette_[i] = rgb565(spectrum_like_color(i));
    pixels_.assign(static_cast<std::size_t>(kWidth) * kCanvasH, rgb565(cp0::theme::kPanel));
    peak_.fill(-120.0f);
    tools_ok_ = tool_present();
    create_ui();
    render();
    if (tools_ok_)
        start_detect();
    else
        show_message("rtl-sdr not installed");
    timer_ = lv_timer_create(timer_cb, kRefreshMs, this);
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

UISdrPage::~UISdrPage()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
    stop_proc();
    if (detect_pid_ > 0) cp0_signal_process_api({"Stop", std::to_string(detect_pid_)}, [](int, std::string) {});
}

// A small green-through-white scale shared with the Spectrum waterfall.
uint32_t UISdrPage::spectrum_like_color(int index)
{
    struct Stop { int at, r, g, b; };
    static constexpr Stop kStops[] = {
        {0, 0, 0, 0}, {12, 0, 0, 0}, {28, 0x13, 0x26, 0x1B}, {46, 0x3D, 0xDC, 0x84}, {63, 0xFF, 0xFF, 0xFF}};
    index = std::clamp(index, 0, 63);
    for (std::size_t i = 1; i < sizeof(kStops) / sizeof(kStops[0]); ++i) {
        const Stop &a = kStops[i - 1];
        const Stop &b = kStops[i];
        if (index <= b.at) {
            const int span = b.at - a.at;
            const int t = index - a.at;
            return static_cast<uint32_t>(((a.r + (b.r - a.r) * t / span) << 16) |
                                         ((a.g + (b.g - a.g) * t / span) << 8) | (a.b + (b.b - a.b) * t / span));
        }
    }
    return 0xFFFFFF;
}

lv_obj_t *UISdrPage::make_label(int x, int y, int px, uint32_t color)
{
    return cp0::ui::label(ui_APP_Container, x, y, color, px > 11);
}

void UISdrPage::create_ui()
{
    if (!ui_APP_Container) return;
    cp0::ui::paint_background(ui_APP_Container);
    const cp0::ui::TitleRow row = cp0::ui::title_row(ui_APP_Container, "sdr");
    title_ = row.title;
    status_ = row.status;

    centre_ = cp0::ui::readout(ui_APP_Container, 4, 19, 186, 40, "centre  MHz");
    gain_ = cp0::ui::readout(ui_APP_Container, 194, 19, 122, 40, "peak  MHz");
    const cp0::ui::Panel body = cp0::ui::panel(ui_APP_Container, 4, 63, 312, 88);

    canvas_ = lv_canvas_create(ui_APP_Container);
    if (canvas_) {
        lv_canvas_set_buffer(canvas_, pixels_.data(), kWidth, kCanvasH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(canvas_, 5, kCanvasTop);
    }
    edge_low_ = cp0::ui::label(ui_APP_Container, 8, kCanvasTop + 1, cp0::theme::kDim);
    edge_high_ = cp0::ui::label(ui_APP_Container, 312 - 4 - 80, kCanvasTop + 1, cp0::theme::kDim);
    if (edge_high_) {
        lv_obj_set_width(edge_high_, 80);
        lv_obj_set_style_text_align(edge_high_, LV_TEXT_ALIGN_RIGHT, 0);
    }
    if (!body.box) return;

    big_ = cp0::ui::label(body.box, 8, 10, kColorText, true);
    sub_ = cp0::ui::label(body.box, 8, 42, kColorIdle);

    list_ = cp0::ui::label(body.box, 8, 6, kColorText);
    if (list_) {
        lv_obj_set_width(list_, 296);
        lv_label_set_long_mode(list_, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_line_space(list_, 4, 0);
    }


    hint_ = cp0::ui::footer(ui_APP_Container);
}

void UISdrPage::show_message(const std::string &text)
{
    message_ = text;
    message_until_ = lv_tick_get() + kMessageMs;
}

void UISdrPage::start_detect()
{
    if (detect_pid_ > 0) return;
    last_detect_ = lv_tick_get();
    detect_pid_ = spawn(sdr::detect_cmd() + " > '" + detect_path_ + "' 2>&1");
}

void UISdrPage::poll_detect()
{
    std::FILE *file = std::fopen(detect_path_.c_str(), "r");
    if (!file) return;
    std::string output;
    char buffer[512];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) output.append(buffer, n);
    std::fclose(file);
    if (output.empty()) return;
    const sdr::Detect detect = sdr::parse_detect(output);
    detect_pid_ = -1;
    if (detect.present && !present_) {
        present_ = true;
        if (mode_ == Mode::Scope) start_scope();
        render();
    } else if (!detect.present && present_) {
        present_ = false;
        stop_proc();
        render();
    }
}

std::string UISdrPage::tail()
{
    std::FILE *file = std::fopen(out_path_.c_str(), "r");
    if (!file) return {};
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    if (size < out_offset_) out_offset_ = 0; // file was replaced
    std::string chunk;
    if (size > out_offset_) {
        std::fseek(file, out_offset_, SEEK_SET);
        chunk.resize(static_cast<std::size_t>(size - out_offset_));
        const std::size_t read = std::fread(&chunk[0], 1, chunk.size(), file);
        chunk.resize(read);
        out_offset_ += static_cast<long>(read);
    }
    std::fclose(file);
    return chunk;
}

void UISdrPage::stop_proc()
{
    if (worker_.joinable()) {
        worker_stop_ = true;
        worker_.join();
    }
    // Close the IQ pipe before signalling: rtl_sdr blocked on a full pipe
    // cannot act on SIGTERM and gets SIGKILLed with the dongle still open,
    // and the next open resets the dongle, which this board does not survive.
    // A closed pipe gives it SIGPIPE, which it handles by closing cleanly.
    if (iq_fd_ >= 0) close(iq_fd_);
    iq_fd_ = -1;
    for (int *pid : {&proc_pid_, &audio_pid_}) {
        if (*pid <= 0) continue;
        const int target = *pid;
        *pid = -1;
        cp0_signal_process_api({"Stop", std::to_string(target)}, [](int, std::string) {});
    }
    unlink(iq_path_.c_str());
    unlink(audio_path_.c_str());
    decoding_ = false;
}

void UISdrPage::start_scope()
{
    stop_proc();
    if (!present_) return;
    peak_.fill(-120.0f);
    smooth_valid_ = false;
    unlink(iq_path_.c_str());
    if (mkfifo(iq_path_.c_str(), 0600) != 0) return;
    // Open the read end first, non-blocking, so neither side waits on the other.
    iq_fd_ = open(iq_path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (iq_fd_ < 0) return;
    proc_pid_ = spawn("exec " + sdr::iq_cmd(center_hz_, gain_db_) + " > '" + iq_path_ + "' 2>/dev/null");
    unlink(audio_path_.c_str());
    if (mkfifo(audio_path_.c_str(), 0600) == 0)
        audio_pid_ = spawn("exec pacat --raw --rate=" + std::to_string(sdr::kAudioRate) +
                           " --channels=1 --format=s16le < '" + audio_path_ + "'");
    {
        std::lock_guard<std::mutex> lock(iq_mutex_);
        iq_latest_.clear();
        iq_sequence_ = iq_drawn_ = 0;
    }
    worker_audio_ = audio_out_;
    worker_demod_ = static_cast<int>(demod_);
    worker_stop_ = false;
    worker_ = std::thread([this] { worker_loop(); });
}

void UISdrPage::worker_loop()
{
    // SIGPIPE from a write is delivered to the writing thread; blocked here,
    // a dead pacat turns into EPIPE instead of killing the launcher.
    sigset_t sigpipe;
    sigemptyset(&sigpipe);
    sigaddset(&sigpipe, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &sigpipe, nullptr);
    const std::size_t keep = static_cast<std::size_t>(2 * sdr::kIqFft);
    int audio_fd = -1;
    int demod = worker_demod_;
    sdr::Demodulator demodulator(static_cast<sdr::Demod>(demod));
    std::vector<uint8_t> buffer(1 << 16);
    std::vector<int16_t> audio;
    while (!worker_stop_) {
        // pacat opens the audio pipe for reading once it starts; until then a
        // non-blocking writer open fails and is retried.
        if (audio_fd < 0) audio_fd = open(audio_path_.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        pollfd pfd{iq_fd_, POLLIN, 0};
        if (poll(&pfd, 1, 100) <= 0) continue;
        const ssize_t n = read(iq_fd_, buffer.data(), buffer.size() & ~static_cast<std::size_t>(1));
        if (n <= 0) continue;
        if (worker_demod_ != demod) {
            demod = worker_demod_;
            demodulator = sdr::Demodulator(static_cast<sdr::Demod>(demod));
        }
        if (worker_audio_ && audio_fd >= 0) {
            audio.clear();
            demodulator.process(buffer.data(), static_cast<std::size_t>(n), audio);
            if (!audio.empty() && write(audio_fd, audio.data(), audio.size() * sizeof(int16_t)) < 0 && errno == EPIPE) {
                close(audio_fd);
                audio_fd = -1;
            }
        }
        std::lock_guard<std::mutex> lock(iq_mutex_);
        if (static_cast<std::size_t>(n) >= keep) {
            iq_latest_.assign(buffer.begin() + (n - static_cast<ssize_t>(keep)), buffer.begin() + n);
        } else {
            iq_latest_.insert(iq_latest_.end(), buffer.begin(), buffer.begin() + n);
            if (iq_latest_.size() > keep) iq_latest_.erase(iq_latest_.begin(), iq_latest_.end() - static_cast<long>(keep));
        }
        ++iq_sequence_;
    }
    if (audio_fd >= 0) close(audio_fd);
}

void UISdrPage::read_iq()
{
    std::vector<uint8_t> block;
    {
        std::lock_guard<std::mutex> lock(iq_mutex_);
        if (iq_sequence_ == iq_drawn_ || iq_latest_.size() < static_cast<std::size_t>(2 * sdr::kIqFft)) return;
        iq_drawn_ = iq_sequence_;
        block = iq_latest_;
    }
    const std::vector<float> bins = sdr::iq_spectrum_db(block.data(), block.size());
    const std::vector<float> cols = sdr::span_columns(bins, span_hz_, kWidth);
    for (int x = 0; x < kWidth; ++x)
        smooth_[x] = smooth_valid_ ? smooth_[x] + kScopeSmoothing * (cols[x] - smooth_[x]) : cols[x];
    smooth_valid_ = true;
    draw_scope(std::vector<float>(smooth_.begin(), smooth_.end()));
}

void UISdrPage::start_decode()
{
    stop_proc();
    if (!present_) return;
    out_offset_ = 0;
    truncate(out_path_.c_str(), 0);
    devices_.clear();
    proc_pid_ = spawn(sdr::decode_cmd(center_hz_, gain_db_, out_path_));
    decoding_ = proc_pid_ > 0;
}

void UISdrPage::retune()
{
    if (mode_ == Mode::Scope && present_)
        start_scope();
    else if (mode_ == Mode::Decode && decoding_)
        start_decode();
    render();
}

void UISdrPage::set_mode(Mode mode)
{
    if (mode == mode_) return;
    stop_proc();
    mode_ = mode;
    if (present_ && mode_ == Mode::Scope) start_scope();
    render();
}

void UISdrPage::draw_scope(const std::vector<float> &cols)
{
    if (!canvas_ || cols.size() < static_cast<std::size_t>(kWidth)) return;
    const uint16_t bg = rgb565(cp0::theme::kPanel);
    const uint16_t grid = rgb565(kColorGrid);
    const uint16_t fill = rgb565(kColorFill);
    const uint16_t accent = rgb565(kColorAccent);
    const uint16_t peak = rgb565(kColorIdle);
    const uint16_t centre = rgb565(cp0::theme::kDim);
    const float floor_db = sdr::median_db(cols) - kScopeBelowFloor;
    auto level_of = [floor_db](float db) {
        return std::clamp(static_cast<int>((db - floor_db) / kScopeRange * 63.0f), 0, 63);
    };
    auto y_for = [](int level) { return (kTraceH - 1) - level * (kTraceH - 1) / 63; };

    // Scroll the waterfall down by one row.
    uint16_t *wf = &pixels_[static_cast<std::size_t>(kTraceH + 1) * kWidth];
    const std::size_t rows = kCanvasH - kTraceH - 1;
    std::memmove(wf + kWidth, wf, (rows - 1) * kWidth * sizeof(uint16_t));

    for (int y = 0; y < kTraceH; ++y) std::fill_n(&pixels_[static_cast<std::size_t>(y) * kWidth], kWidth, bg);
    for (int y = 0; y < kTraceH; y += 2) pixels_[static_cast<std::size_t>(y) * kWidth + kWidth / 2] = centre;
    int best = 0;
    for (int x = 0; x < kWidth; ++x) {
        const int level = level_of(cols[x]);
        const int top = y_for(level);
        for (int y = top + 1; y < kTraceH; ++y) pixels_[static_cast<std::size_t>(y) * kWidth + x] = fill;
        pixels_[static_cast<std::size_t>(top) * kWidth + x] = accent;
        peak_[x] = std::max(peak_[x], cols[x]);
        const int peak_y = y_for(level_of(peak_[x]));
        if (peak_y < top) pixels_[static_cast<std::size_t>(peak_y) * kWidth + x] = peak;
        // The waterfall spans 30 dB above the floor and starts at a dark green,
        // so the noise floor shows instead of disappearing into black.
        const float over = (cols[x] - floor_db - kScopeBelowFloor) / 30.0f;
        wf[x] = palette_[std::clamp(22 + static_cast<int>(over * 41.0f), 0, 63)];
        if (cols[x] > cols[best]) best = x;
    }
    for (int x = 0; x < kWidth; ++x) pixels_[static_cast<std::size_t>(kTraceH) * kWidth + x] = grid;
    lv_obj_invalidate(canvas_);

    const float above = cols[best] - (floor_db + kScopeBelowFloor);
    const double hz = static_cast<double>(center_hz_) - span_hz_ / 2.0 + (best + 0.5) * span_hz_ / kWidth;
    char caption[24];
    std::snprintf(caption, sizeof(caption), "peak  +%d dB", static_cast<int>(std::lround(above)));
    if (gain_.label) cp0::ui::set_text(gain_.label, caption, cp0::theme::kDim);
    cp0::ui::set_readout(gain_, sdr::format_mhz(static_cast<uint32_t>(hz)), "", above >= 20.0f ? kColorAccent : kColorIdle);
}

void UISdrPage::render()
{
    const bool scope = mode_ == Mode::Scope;
    const bool decode = mode_ == Mode::Decode;
    const bool show_scope = scope && present_;
    const bool show_decode = decode && present_;

    auto vis = [](lv_obj_t *obj, bool on) {
        if (!obj) return;
        if (on) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    };
    vis(canvas_, show_scope);
    vis(big_, !present_);
    vis(sub_, !present_);
    vis(list_, show_decode);

    cp0::ui::set_readout(centre_, sdr::format_mhz(center_hz_), "", kColorText);
    if (centre_.label)
        cp0::ui::set_text(centre_.label, ("step " + sdr::format_hz(sdr::kSteps[step_index_])).c_str(),
                          cp0::theme::kDim);
    vis(edge_low_, show_scope);
    vis(edge_high_, show_scope);
    vis(gain_.panel.box, show_scope);
    if (show_scope) {
        cp0::ui::set_text(edge_low_, sdr::format_mhz(center_hz_ - span_hz_ / 2).c_str(), cp0::theme::kDim);
        cp0::ui::set_text(edge_high_, sdr::format_mhz(center_hz_ + span_hz_ / 2).c_str(), cp0::theme::kDim);
    }
    if (status_) {
        std::string text;
        uint32_t color = kColorIdle;
        if (lv_tick_get() < message_until_) {
            text = message_;
            color = kColorText;
        } else if (!tools_ok_) {
            text = "not installed";
            color = kColorRec;
        } else if (!present_) {
            text = "no dongle";
        } else if (decoding_) {
            text = "decoding";
            color = kColorRec;
        } else if (scope) {
            text = std::string(sdr::demod_label(demod_)) + (audio_out_ ? "" : " muted") + "  g " +
                   sdr::gain_label(gain_db_);
            color = audio_out_ ? kColorAccent : kColorIdle;
        } else {
            text = "decode  g " + sdr::gain_label(gain_db_);
        }
        lv_label_set_text(status_, text.c_str());
        cp0::ui::set_text_color(status_, color);
    }

    if (!present_) {
        if (big_) lv_label_set_text(big_, "no sdr");
        if (sub_) lv_label_set_text(sub_, tools_ok_ ? "plug in the rtl-sdr dongle" : "run: sudo apt install rtl-sdr");
    }

    if (decode && list_) {
        std::string body;
        if (!decoding_)
            body = "enter to start rtl_433";
        else if (devices_.empty())
            body = "listening...";
        else
            for (const std::string &d : devices_) body += d + "\n";
        lv_label_set_text(list_, body.c_str());
    }

    if (!present_)
        cp0::ui::set_footer(hint_, {{"tab", "mode"}});
    else if (scope)
        cp0::ui::set_footer(hint_, {{"< >", "tune"}, {"m", "demod"}, {"o", audio_out_ ? "mute" : "unmute"},
                                    {"up/dn", "step"}, {"s", "span"}, {"g", "gain"}});
    else
        cp0::ui::set_footer(hint_, {{"< >", "tune"}, {"ent", "start"}, {"s", "save"}, {"tab", "mode"}});
}

void UISdrPage::timer_cb(lv_timer_t *timer) noexcept
{
    auto *page = static_cast<UISdrPage *>(lv_timer_get_user_data(timer));
    if (!page) return;
    try {
        page->on_timer();
    } catch (...) {
    }
}

void UISdrPage::on_timer()
{
    if (tools_ok_ && !present_ && detect_pid_ <= 0 && lv_tick_get() - last_detect_ >= kDetectEveryMs)
        start_detect();
    if (detect_pid_ > 0) poll_detect();

    if (present_ && mode_ == Mode::Scope && proc_pid_ > 0) {
        read_iq();
    } else if (present_ && mode_ == Mode::Decode && decoding_) {
        const std::string chunk = tail();
        std::size_t start = 0;
        bool added = false;
        while (true) {
            const std::size_t nl = chunk.find('\n', start);
            if (nl == std::string::npos) break;
            const std::string summary = sdr::summarize_433(chunk.substr(start, nl - start));
            start = nl + 1;
            if (summary.empty()) continue;
            devices_.erase(std::remove_if(devices_.begin(), devices_.end(),
                                          [&](const std::string &d) {
                                              return d.compare(0, summary.find(' '), summary, 0,
                                                                summary.find(' ')) == 0;
                                          }),
                           devices_.end());
            devices_.push_front(summary);
            if (devices_.size() > kMaxDevices) devices_.pop_back();
            added = true;
        }
        if (added) render();
    }

    if (message_until_ != 0 && lv_tick_get() >= message_until_) {
        message_until_ = 0;
        render();
    }
}

void UISdrPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UISdrPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item) return;
    const uint32_t key = item->semantic_key ? item->semantic_key : item->key_code;
    try {
        page->handle_key(key, item->key_state == KBD_KEY_REPEATED);
    } catch (...) {
    }
}

void UISdrPage::handle_key(uint32_t key, bool repeat)
{
    if (repeat && key != KEY_LEFT && key != KEY_RIGHT) return;
    if (key == KEY_ESC) {
        if (navigate_home) navigate_home();
        return;
    }
    if (key == KEY_TAB) {
        set_mode(mode_ == Mode::Scope ? Mode::Decode : Mode::Scope);
        return;
    }
    if (key == KEY_G) {
        static const int kGains[] = {-1, 20, 30, 40};
        int next = 0;
        for (int i = 0; i < 4; ++i)
            if (kGains[i] == gain_db_) next = (i + 1) % 4;
        gain_db_ = kGains[next];
        show_message("gain " + sdr::gain_label(gain_db_));
        retune();
        return;
    }
    if (key == KEY_LEFT || key == KEY_RIGHT) {
        const int64_t delta = static_cast<int64_t>(sdr::kSteps[step_index_]) * (key == KEY_RIGHT ? 1 : -1);
        center_hz_ = sdr::clamp_hz(static_cast<int64_t>(center_hz_) + delta);
        retune();
        return;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        const int count = static_cast<int>(sdr::kSteps.size());
        step_index_ = std::clamp(step_index_ + (key == KEY_UP ? 1 : -1), 0, count - 1);
        render();
        return;
    }
    if (key >= KEY_1 && key <= KEY_6) {
        const std::size_t index = key - KEY_1;
        if (index < sdr::kPresets.size()) {
            const sdr::Preset &preset = sdr::kPresets[index];
            center_hz_ = preset.hz;
            demod_ = preset.demod;
            span_hz_ = preset.span_hz;
            show_message(std::string(preset.label) + " " + sdr::format_hz(center_hz_));
            retune();
        }
        return;
    }

    if (mode_ == Mode::Scope) {
        if (key == KEY_S) {
            span_hz_ = span_hz_ >= 2000000u ? 1000000u : span_hz_ >= 1000000u ? 500000u : span_hz_ >= 500000u ? 250000u : 2000000u;
            show_message("span " + sdr::format_hz(span_hz_));
            if (present_) start_scope();
            render();
        } else if (key == KEY_H) {
            peak_.fill(-120.0f);
        } else if (key == KEY_M) {
            demod_ = demod_ == sdr::Demod::WFM ? sdr::Demod::NFM
                     : demod_ == sdr::Demod::NFM ? sdr::Demod::AM
                                                 : sdr::Demod::WFM;
            worker_demod_ = static_cast<int>(demod_);
            render();
        } else if (key == KEY_O) {
            audio_out_ = !audio_out_;
            worker_audio_ = audio_out_;
            set_config_audio_out(audio_out_);
            render();
        }
        return;
    }
    if (mode_ == Mode::Decode) {
        if (key == KEY_ENTER || key == KEY_KPENTER) {
            if (decoding_) {
                stop_proc();
                render();
            } else {
                start_decode();
                render();
            }
        } else if (key == KEY_S && !devices_.empty()) {
            const std::string path = home_dir() + "/spectrum/sdr-devices.log";
            if (std::FILE *f = std::fopen(path.c_str(), "a")) {
                for (const std::string &d : devices_) std::fprintf(f, "%s\n", d.c_str());
                std::fclose(f);
                show_message("saved log");
            }
        }
        return;
    }
}

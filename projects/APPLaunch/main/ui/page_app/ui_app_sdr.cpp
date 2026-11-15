/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_sdr.hpp"

#include "cp0_lvgl_app.h"
#include "input_keys.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {

constexpr uint32_t kColorAccent = 0x3DDC84;
constexpr uint32_t kColorText = 0xE6E6E6;
constexpr uint32_t kColorIdle = 0x8A8A8A;
constexpr uint32_t kColorDim = 0x5F7A66;
constexpr uint32_t kColorGrid = 0x1E2B23;
constexpr uint32_t kColorFill = 0x0D2216;
constexpr uint32_t kColorRec = 0xFF5F56;
constexpr uint32_t kRefreshMs = 200;
constexpr uint32_t kMessageMs = 3000;
constexpr uint32_t kDetectEveryMs = 3000;
constexpr int kScopeFloor = -75;
constexpr int kScopeCeil = -15;
constexpr int kMaxDevices = 8;

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
    detect_path_ = runtime_dir() + "/cp0-sdr-detect";
    out_path_ = runtime_dir() + "/cp0-sdr-out";
    for (int i = 0; i < 64; ++i)
        palette_[i] = rgb565(spectrum_like_color(i));
    pixels_.assign(static_cast<std::size_t>(kWidth) * kCanvasH, rgb565(0x000000));
    peak_.fill(static_cast<int8_t>(kScopeFloor));
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
    lv_obj_t *label = lv_label_create(ui_APP_Container);
    if (!label) return nullptr;
    lv_obj_set_style_text_font(label, launcher_fonts().get_chrome(px), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    lv_label_set_text(label, "");
    return label;
}

void UISdrPage::create_ui()
{
    if (!ui_APP_Container) return;
    lv_obj_set_style_bg_color(ui_APP_Container, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_APP_Container, LV_OPA_COVER, 0);

    title_ = make_label(4, 1, 11, kColorIdle);
    status_ = make_label(200, 1, 11, kColorIdle);
    if (status_) {
        lv_obj_set_width(status_, 116);
        lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_RIGHT, 0);
    }

    canvas_ = lv_canvas_create(ui_APP_Container);
    if (canvas_) {
        lv_canvas_set_buffer(canvas_, pixels_.data(), kWidth, kCanvasH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(canvas_, 0, kCanvasTop);
    }

    big_ = make_label(14, 40, 22, kColorText);
    sub_ = make_label(16, 70, 11, kColorDim);

    list_ = make_label(6, kCanvasTop, 11, kColorText);
    if (list_) {
        lv_obj_set_width(list_, 308);
        lv_label_set_long_mode(list_, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_line_space(list_, 4, 0);
    }

    hint_ = make_label(4, kFooterY, 11, kColorDim);
    if (hint_) lv_obj_set_width(hint_, 312);
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
    if (proc_pid_ > 0) {
        const int pid = proc_pid_;
        proc_pid_ = -1;
        cp0_signal_process_api({"Stop", std::to_string(pid)}, [](int, std::string) {});
    }
    playing_ = false;
    decoding_ = false;
}

void UISdrPage::start_scope()
{
    stop_proc();
    if (!present_) return;
    out_offset_ = 0;
    truncate(out_path_.c_str(), 0);
    peak_.fill(static_cast<int8_t>(kScopeFloor));
    proc_pid_ = spawn(sdr::power_cmd(center_hz_, span_hz_, gain_db_, out_path_));
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

void UISdrPage::toggle_listen()
{
    if (playing_) {
        stop_proc();
        show_message("stopped");
        render();
        return;
    }
    if (!present_) return;
    proc_pid_ = spawn(sdr::listen_cmd(center_hz_, demod_, gain_db_));
    playing_ = proc_pid_ > 0;
    render();
}

void UISdrPage::retune()
{
    if (mode_ == Mode::Scope && present_)
        start_scope();
    else if (mode_ == Mode::Decode && decoding_)
        start_decode();
    else if (mode_ == Mode::Listen && playing_) {
        stop_proc();
        toggle_listen();
    }
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

void UISdrPage::draw_scope(const sdr::PowerRow &row)
{
    if (!canvas_) return;
    const std::vector<int> cols = sdr::power_columns(row, kWidth);
    const uint16_t black = rgb565(0x000000);
    const uint16_t grid = rgb565(kColorGrid);
    const uint16_t fill = rgb565(kColorFill);
    const uint16_t accent = rgb565(kColorAccent);
    const uint16_t peak = rgb565(kColorIdle);
    auto y_for = [](int level) { return (kTraceH - 1) - level * (kTraceH - 1) / 63; };

    // Scroll the waterfall down by one row.
    uint16_t *wf = &pixels_[static_cast<std::size_t>(kTraceH + 1) * kWidth];
    const std::size_t rows = kCanvasH - kTraceH - 1;
    std::memmove(wf + kWidth, wf, (rows - 1) * kWidth * sizeof(uint16_t));

    for (int y = 0; y < kTraceH; ++y) std::fill_n(&pixels_[static_cast<std::size_t>(y) * kWidth], kWidth, black);
    for (int x = 0; x < kWidth; ++x) {
        const int level = sdr::level_index(cols[x], kScopeFloor, kScopeCeil, 64);
        const int top = y_for(level);
        for (int y = top + 1; y < kTraceH; ++y) pixels_[static_cast<std::size_t>(y) * kWidth + x] = fill;
        pixels_[static_cast<std::size_t>(top) * kWidth + x] = accent;
        peak_[x] = static_cast<int8_t>(std::max<int>(peak_[x], cols[x]));
        const int peak_y = y_for(sdr::level_index(peak_[x], kScopeFloor, kScopeCeil, 64));
        if (peak_y < top) pixels_[static_cast<std::size_t>(peak_y) * kWidth + x] = peak;
        wf[x] = palette_[level];
    }
    for (int x = 0; x < kWidth; ++x) pixels_[static_cast<std::size_t>(kTraceH) * kWidth + x] = grid;
    lv_obj_invalidate(canvas_);
}

void UISdrPage::render()
{
    const bool scope = mode_ == Mode::Scope;
    const bool listen = mode_ == Mode::Listen;
    const bool decode = mode_ == Mode::Decode;
    const bool show_scope = scope && present_;
    const bool show_listen = listen && present_;
    const bool show_decode = decode && present_;

    auto vis = [](lv_obj_t *obj, bool on) {
        if (!obj) return;
        if (on) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    };
    vis(canvas_, show_scope);
    vis(big_, show_listen || (!present_));
    vis(sub_, show_listen || (!present_));
    vis(list_, show_decode);

    const char *mode_name = scope ? "scope" : listen ? "listen" : "decode";
    if (title_) {
        std::string text = std::string(mode_name) + "  " + sdr::format_hz(center_hz_);
        lv_label_set_text(title_, text.c_str());
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
        } else if (playing_ || decoding_) {
            text = "live";
            color = kColorRec;
        } else {
            text = "g " + sdr::gain_label(gain_db_);
        }
        lv_label_set_text(status_, text.c_str());
        lv_obj_set_style_text_color(status_, lv_color_hex(color), 0);
    }

    if (!present_) {
        if (big_) lv_label_set_text(big_, "no sdr");
        if (sub_) lv_label_set_text(sub_, tools_ok_ ? "plug in the rtl-sdr dongle" : "run: sudo apt install rtl-sdr");
    } else if (listen) {
        if (big_) lv_label_set_text(big_, (sdr::format_mhz(center_hz_) + " MHz").c_str());
        if (sub_) {
            std::string s = std::string(sdr::demod_label(demod_)) + (playing_ ? "   playing" : "   stopped");
            lv_label_set_text(sub_, s.c_str());
            lv_obj_set_style_text_color(sub_, lv_color_hex(playing_ ? kColorAccent : kColorDim), 0);
        }
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

    if (hint_) {
        const char *text = !present_ ? "tab mode   esc back"
                           : scope   ? "< > tune  , . step  g gain  s span  tab"
                           : listen  ? "< > tune  1-6 preset  m mode  ent play"
                                     : "< > tune  ent start/stop  s save  tab";
        lv_label_set_text(hint_, text);
    }
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
        const std::string chunk = tail();
        std::size_t start = 0;
        std::string last;
        while (true) {
            const std::size_t nl = chunk.find('\n', start);
            if (nl == std::string::npos) break;
            last = chunk.substr(start, nl - start);
            start = nl + 1;
        }
        if (!last.empty()) {
            const sdr::PowerRow row = sdr::parse_power_row(last);
            if (row.valid) draw_scope(row);
        }
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
        set_mode(mode_ == Mode::Scope ? Mode::Listen : mode_ == Mode::Listen ? Mode::Decode : Mode::Scope);
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
    if (key == KEY_COMMA || key == KEY_DOT) {
        const int count = static_cast<int>(sdr::kSteps.size());
        step_index_ = (step_index_ + (key == KEY_DOT ? 1 : count - 1)) % count;
        show_message("step " + sdr::format_hz(sdr::kSteps[step_index_]));
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
            span_hz_ = span_hz_ >= 2000000u ? 1000000u : 2000000u;
            show_message("span " + sdr::format_hz(span_hz_));
            if (present_) start_scope();
            render();
        } else if (key == KEY_H) {
            peak_.fill(static_cast<int8_t>(kScopeFloor));
        }
        return;
    }
    if (mode_ == Mode::Listen) {
        if (key == KEY_M) {
            demod_ = demod_ == sdr::Demod::WFM ? sdr::Demod::NFM
                     : demod_ == sdr::Demod::NFM ? sdr::Demod::AM
                                                 : sdr::Demod::WFM;
            show_message(sdr::demod_label(demod_));
            if (playing_) toggle_listen(), toggle_listen();
            render();
        } else if (key == KEY_ENTER || key == KEY_KPENTER) {
            toggle_listen();
        } else if (key == KEY_SPACE) {
            if (playing_) toggle_listen();
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

/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_spectrum.hpp"

#include "cp0_lvgl_app.h"
#include "input_keys.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>

namespace {

constexpr uint32_t kColorAccent = 0x3DDC84;
constexpr uint32_t kColorText = 0xE6E6E6;
constexpr uint32_t kColorIdle = 0x8A8A8A;
constexpr uint32_t kColorDim = 0x5F7A66;
constexpr uint32_t kColorGrid = 0x1E2B23;
constexpr uint32_t kColorFill = 0x0D2216;
constexpr const char *kBandConfigKey = "spectrum_band";
// Peaks above this level count as activity in the band selector.
constexpr int kActivityDbm = -85;
constexpr uint32_t kRefreshMs = 50;
constexpr uint32_t kMessageMs = 3000;

uint16_t rgb565(uint32_t rgb)
{
    return lv_color_to_u16(lv_color_hex(rgb));
}

int bin_for_x(int x)
{
    return x * spectrum::kBins / 320;
}

int x_for_bin(int bin)
{
    return bin * 320 / spectrum::kBins + 320 / spectrum::kBins / 2;
}

int config_band()
{
    int value = 0;
    cp0_signal_config_api({"GetInt", kBandConfigKey, "0"}, [&](int code, std::string data) {
        if (code == 0) value = std::atoi(data.c_str());
    });
    return value;
}

// Save writes and fsyncs the config file, so it waits until the page closes.
void set_config_band(int value)
{
    cp0_signal_config_api({"SetInt", kBandConfigKey, std::to_string(value)}, nullptr);
}

void save_config()
{
    cp0_signal_config_api({"Save"}, nullptr);
}

// The timer refreshes labels 20 times a second; unchanged text or colour must
// not invalidate them.
void set_text(lv_obj_t *label, const char *text)
{
    if (!label) return;
    const char *current = lv_label_get_text(label);
    if (current && std::strcmp(current, text) == 0) return;
    lv_label_set_text(label, text);
}

void set_color(lv_obj_t *obj, uint32_t color)
{
    if (!obj) return;
    const lv_color_t value = lv_color_hex(color);
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), value))
        lv_obj_set_style_text_color(obj, value, LV_PART_MAIN);
}

std::string read_file(const std::string &path)
{
    std::string out;
    if (std::FILE *file = std::fopen(path.c_str(), "r")) {
        char buffer[4096];
        std::size_t count = 0;
        while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) out.append(buffer, count);
        std::fclose(file);
    }
    return out;
}

} // namespace

UISpectrumPage::UISpectrumPage() : AppPage()
{
    set_page_title("Spectrum");
    for (int i = 0; i < spectrum::kLevels; ++i) palette_[i] = rgb565(spectrum::level_color(i));
    pixels_.assign(static_cast<std::size_t>(kWidth) * kCanvasH, rgb565(0x000000));
    band_ = static_cast<std::size_t>(std::clamp(config_band(), 0, static_cast<int>(spectrum::kBands.size()) - 1));
    band_peak_.fill(spectrum::kMinDbm);
    create_ui();

    scanner_.set_band(band_);
    std::string error;
    scanner_ok_ = scanner_.start(error);
    if (!scanner_ok_) show_message(error);
    reset_view();

    timer_ = lv_timer_create(timer_cb, kRefreshMs, this);
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

UISpectrumPage::~UISpectrumPage()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
    scanner_.stop();
    if (band_changed_) save_config();
}

lv_obj_t *UISpectrumPage::make_label(int x, int y, uint32_t color)
{
    lv_obj_t *label = lv_label_create(ui_APP_Container);
    if (!label) return nullptr;
    lv_obj_set_style_text_font(label, launcher_fonts().get_chrome(11), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    lv_label_set_text(label, "");
    return label;
}

void UISpectrumPage::create_ui()
{
    if (!ui_APP_Container) return;
    lv_obj_set_style_bg_color(ui_APP_Container, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_APP_Container, LV_OPA_COVER, 0);

    for (std::size_t i = 0; i < band_labels_.size(); ++i)
        band_labels_[i] = make_label(4 + static_cast<int>(i) * 70, 1, kColorIdle);
    status_label_ = make_label(220, 1, kColorIdle);
    if (status_label_) {
        lv_obj_set_width(status_label_, 96);
        lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_RIGHT, 0);
    }

    canvas_ = lv_canvas_create(ui_APP_Container);
    if (canvas_) {
        lv_canvas_set_buffer(canvas_, pixels_.data(), kWidth, kCanvasH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(canvas_, 0, kCanvasTop);
    }

    list_label_ = make_label(4, kCanvasTop + 2, kColorText);
    if (list_label_) {
        lv_obj_set_width(list_label_, 312);
        lv_label_set_long_mode(list_label_, LV_LABEL_LONG_CLIP);
        lv_obj_add_flag(list_label_, LV_OBJ_FLAG_HIDDEN);
    }

    marker_label_ = make_label(4, kFooterY, kColorText);
    hint_label_ = make_label(150, kFooterY, kColorDim);
    if (hint_label_) {
        lv_obj_set_width(hint_label_, 166);
        lv_obj_set_style_text_align(hint_label_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(hint_label_, "tab band ent cap r rec");
    }
}

void UISpectrumPage::reset_view()
{
    latest_.fill(static_cast<int8_t>(spectrum::kMinDbm));
    peak_.fill(static_cast<int8_t>(spectrum::kMinDbm));
    std::fill(pixels_.begin(), pixels_.end(), rgb565(0x000000));
    shown_sequence_ = scanner_.snapshot().sequence[band_];
    draw_trace();
    update_labels();
}

void UISpectrumPage::select_band(std::size_t index)
{
    if (index >= spectrum::kBands.size() || index == band_) return;
    band_ = index;
    scanner_.set_band(band_);
    set_config_band(static_cast<int>(band_));
    band_changed_ = true;
    reset_view();
}

void UISpectrumPage::build_send_entries()
{
    send_entries_.clear();
    const char *home = std::getenv("HOME");
    const std::string directory = std::string(home && home[0] ? home : "/tmp") + "/spectrum";
    DIR *dir = opendir(directory.c_str());
    if (!dir) return;
    std::vector<std::string> names;
    while (dirent *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".sub") == 0) names.push_back(name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) { return a > b; });
    constexpr std::size_t kMaxEntries = 24;
    if (names.size() > kMaxEntries) names.resize(kMaxEntries);
    for (const std::string &name : names) {
        const std::string path = directory + "/" + name;
        const spectrum::Capture capture = spectrum::parse_sub(read_file(path));
        SendEntry entry;
        entry.path = path;
        entry.freq_hz = capture.freq_hz;
        entry.allowed = spectrum::tx_allowed(capture.freq_hz) && !capture.durations.empty();
        std::string label = name;
        if (label.rfind("capture-", 0) == 0) label = label.substr(8);
        if (label.size() > 4) label.resize(label.size() - 4);
        entry.label = std::move(label);
        send_entries_.push_back(std::move(entry));
    }
}

void UISpectrumPage::refresh_send_list()
{
    if (!list_label_) return;
    if (send_entries_.empty()) {
        lv_label_set_text(list_label_, "no captures in ~/spectrum\n\nrecord one with r, then esc");
        return;
    }
    send_selection_ = std::clamp(send_selection_, 0, static_cast<int>(send_entries_.size()) - 1);
    constexpr int kRows = 9;
    int first = std::max(0, send_selection_ - kRows / 2);
    if (first + kRows > static_cast<int>(send_entries_.size()))
        first = std::max(0, static_cast<int>(send_entries_.size()) - kRows);
    std::string text;
    for (int i = first; i < static_cast<int>(send_entries_.size()) && i < first + kRows; ++i) {
        const SendEntry &entry = send_entries_[i];
        text += i == send_selection_ ? ">" : " ";
        text += entry.allowed ? " " : "x";
        text += entry.label;
        text += "\n";
    }
    lv_label_set_text(list_label_, text.c_str());
}

void UISpectrumPage::enter_send_view()
{
    build_send_entries();
    send_selection_ = 0;
    send_view_ = true;
    if (canvas_) lv_obj_add_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
    if (list_label_) lv_obj_remove_flag(list_label_, LV_OBJ_FLAG_HIDDEN);
    refresh_send_list();
    update_labels();
}

void UISpectrumPage::exit_send_view()
{
    send_view_ = false;
    if (list_label_) lv_obj_add_flag(list_label_, LV_OBJ_FLAG_HIDDEN);
    if (canvas_) lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
    draw_trace();
    update_labels();
}

void UISpectrumPage::show_message(const std::string &text)
{
    message_ = text;
    message_until_ = lv_tick_get() + kMessageMs;
}

void UISpectrumPage::timer_cb(lv_timer_t *timer) noexcept
{
    auto *page = static_cast<UISpectrumPage *>(lv_timer_get_user_data(timer));
    if (!page) return;
    try {
        page->on_timer();
    } catch (...) {
    }
}

void UISpectrumPage::on_timer()
{
    const std::string message = scanner_.take_message();
    if (!message.empty()) show_message(message);

    if (send_view_) {
        update_labels();
        return;
    }

    capture_ = scanner_.capture_status();
    if (capture_.active) {
        capture_view_ = true;
        if (capture_.sequence != shown_capture_sequence_) {
            shown_capture_sequence_ = capture_.sequence;
            draw_capture(capture_.last);
        }
        update_labels();
        return;
    }
    capture_view_ = false;

    const SpectrumFrame frame = scanner_.snapshot();
    for (std::size_t i = 0; i < frame.dbm.size(); ++i) {
        if (frame.sequence[i] == 0) continue;
        band_peak_[i] = *std::max_element(frame.dbm[i].begin(), frame.dbm[i].end());
    }
    if (frame.sequence[band_] != shown_sequence_) {
        shown_sequence_ = frame.sequence[band_];
        latest_ = frame.dbm[band_];
        for (int bin = 0; bin < spectrum::kBins; ++bin) peak_[bin] = std::max(peak_[bin], latest_[bin]);
        push_waterfall_row();
        draw_trace();
    }
    update_labels();
}

void UISpectrumPage::draw_trace()
{
    const uint16_t black = rgb565(0x000000);
    const uint16_t grid = rgb565(kColorGrid);
    const uint16_t fill = rgb565(kColorFill);
    const uint16_t accent = rgb565(kColorAccent);
    const uint16_t peak = rgb565(kColorIdle);
    const uint16_t marker = rgb565(kColorDim);
    auto y_for = [](int dbm) {
        return (kTraceH - 1) - spectrum::level_index(dbm) * (kTraceH - 1) / (spectrum::kLevels - 1);
    };

    for (int y = 0; y < kTraceH; ++y) std::fill_n(&pixels_[static_cast<std::size_t>(y) * kWidth], kWidth, black);
    for (int dbm = -100; dbm <= -50; dbm += 10) {
        const int y = y_for(dbm);
        for (int x = 0; x < kWidth; x += 2) pixels_[static_cast<std::size_t>(y) * kWidth + x] = grid;
    }
    for (int x = 0; x < kWidth; ++x) {
        const int bin = bin_for_x(x);
        const int top = y_for(latest_[bin]);
        for (int y = top + 1; y < kTraceH; ++y) pixels_[static_cast<std::size_t>(y) * kWidth + x] = fill;
        pixels_[static_cast<std::size_t>(top) * kWidth + x] = accent;
        const int peak_y = y_for(peak_[bin]);
        if (peak_y < top) pixels_[static_cast<std::size_t>(peak_y) * kWidth + x] = peak;
    }
    const int mx = x_for_bin(marker_);
    // Only in the trace: marker pixels in the waterfall would scroll down with it.
    for (int y = 0; y < kTraceH; y += 3) pixels_[static_cast<std::size_t>(y) * kWidth + mx] = marker;
    for (int x = 0; x < kWidth; ++x) pixels_[static_cast<std::size_t>(kTraceH) * kWidth + x] = grid;
    if (canvas_) lv_obj_invalidate(canvas_);
}

// Last captured transmission as a two-level waveform across the trace area.
void UISpectrumPage::draw_capture(const std::vector<int32_t> &durations)
{
    const uint16_t black = rgb565(0x000000);
    const uint16_t grid = rgb565(kColorGrid);
    const uint16_t accent = rgb565(kColorAccent);
    constexpr int kHighY = 12;
    constexpr int kLowY = 38;
    for (int y = 0; y < kTraceH; ++y) std::fill_n(&pixels_[static_cast<std::size_t>(y) * kWidth], kWidth, black);
    for (int x = 0; x < kWidth; ++x) pixels_[static_cast<std::size_t>(kTraceH) * kWidth + x] = grid;

    int64_t total = 0;
    for (int32_t value : durations) total += value > 0 ? value : -value;
    if (total > 0) {
        // Fit the packet to the width, but never finer than 5 us per pixel.
        const int64_t us_per_px = std::max<int64_t>(5, (total + kWidth - 1) / kWidth);
        int64_t t = 0;
        int previous_y = kLowY;
        for (int32_t value : durations) {
            const int y = value > 0 ? kHighY : kLowY;
            const int x0 = static_cast<int>(t / us_per_px);
            t += value > 0 ? value : -value;
            const int x1 = std::min(kWidth - 1, static_cast<int>(t / us_per_px));
            if (x0 >= kWidth) break;
            for (int yy = std::min(previous_y, y); yy <= std::max(previous_y, y); ++yy)
                pixels_[static_cast<std::size_t>(yy) * kWidth + x0] = accent;
            for (int x = x0; x <= x1; ++x) pixels_[static_cast<std::size_t>(y) * kWidth + x] = accent;
            previous_y = y;
        }
    }
    if (canvas_) lv_obj_invalidate(canvas_);
}

void UISpectrumPage::push_waterfall_row()
{
    uint16_t *top = &pixels_[static_cast<std::size_t>(kWaterfallTop) * kWidth];
    const std::size_t rows = kCanvasH - kWaterfallTop;
    std::memmove(top + kWidth, top, (rows - 1) * kWidth * sizeof(uint16_t));
    for (int x = 0; x < kWidth; ++x) top[x] = palette_[spectrum::level_index(latest_[bin_for_x(x)])];
}

void UISpectrumPage::update_labels()
{
    for (std::size_t i = 0; i < band_labels_.size(); ++i) {
        if (!band_labels_[i]) continue;
        const bool selected = i == band_;
        char text[24];
        if (selected)
            std::snprintf(text, sizeof(text), "[%s]", spectrum::kBands[i].label);
        else
            std::snprintf(text, sizeof(text), " %s ", spectrum::kBands[i].label);
        set_text(band_labels_[i], text);
        const bool active = band_peak_[i] > kActivityDbm;
        set_color(band_labels_[i], selected ? (active ? kColorAccent : kColorText) : kColorDim);
    }

    if (status_label_) {
        std::string status;
        uint32_t color = kColorIdle;
        if (capture_view_) {
            char text[32];
            std::snprintf(text, sizeof(text), "cap %d/%d", capture_.rssi_dbm, capture_.threshold_dbm);
            status = text;
            color = capture_.rssi_dbm >= capture_.threshold_dbm ? kColorAccent : kColorIdle;
        } else if (lv_tick_get() < message_until_ || !scanner_ok_) {
            status = message_;
            color = scanner_ok_ ? kColorText : 0xFF5F56;
        } else if (send_view_) {
            status = "send";
        } else if (scanner_.recording()) {
            const int64_t seconds = (std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch()).count() -
                                     scanner_.recording_started_ms()) / 1000;
            char text[24];
            std::snprintf(text, sizeof(text), "rec %02lld:%02lld", static_cast<long long>(seconds / 60),
                          static_cast<long long>(seconds % 60));
            status = text;
            color = 0xFF5F56;
        } else if (scanner_.paused()) {
            status = "paused";
        } else {
            char text[24];
            std::snprintf(text, sizeof(text), "peak %d", band_peak_[band_]);
            status = text;
        }
        set_text(status_label_, status.c_str());
        set_color(status_label_, color);
    }

    if (hint_label_)
        set_text(hint_label_, send_view_      ? "up/dn pick  ent send  esc"
                                       : capture_view_ ? "ent stop  -/= thr"
                                                       : "tab band  ent cap  p send");
    if (marker_label_ && send_view_) {
        if (send_entries_.empty()) {
            set_text(marker_label_, "");
        } else {
            const SendEntry &entry =
                send_entries_[std::clamp(send_selection_, 0, static_cast<int>(send_entries_.size()) - 1)];
            char text[48];
            std::snprintf(text, sizeof(text), "%s MHz  %s", spectrum::format_mhz(entry.freq_hz).c_str(),
                          entry.allowed ? "ok" : "blocked");
            set_text(marker_label_, text);
        }
    } else if (marker_label_ && capture_view_) {
        char text[48];
        std::snprintf(text, sizeof(text), "%s MHz %u pkts", spectrum::format_mhz(capture_.freq_hz).c_str(),
                      capture_.packets);
        set_text(marker_label_, text);
    } else if (marker_label_) {
        const uint32_t hz = spectrum::bin_frequency(spectrum::kBands[band_], marker_);
        char text[40];
        std::snprintf(text, sizeof(text), "%s MHz %d dBm", spectrum::format_mhz(hz).c_str(), latest_[marker_]);
        set_text(marker_label_, text);
    }
}

void UISpectrumPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UISpectrumPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item) return;
    // Arrows are Fn combinations on this keyboard; semantic_key carries the arrow.
    const uint32_t key = item->semantic_key ? item->semantic_key : item->key_code;
    const bool repeat = item->key_state == KBD_KEY_REPEATED;
    if (repeat && key != KEY_LEFT && key != KEY_RIGHT) return;
    try {
        page->handle_key(key);
    } catch (...) {
    }
}

void UISpectrumPage::handle_key(uint32_t key)
{
    if (send_view_) {
        switch (key) {
        case KEY_ESC:
            exit_send_view();
            return;
        case KEY_UP:
            if (send_selection_ > 0) {
                --send_selection_;
                refresh_send_list();
                update_labels();
            }
            return;
        case KEY_DOWN:
        case KEY_TAB:
            if (send_selection_ + 1 < static_cast<int>(send_entries_.size())) {
                ++send_selection_;
                refresh_send_list();
                update_labels();
            }
            return;
        case KEY_ENTER:
        case KEY_KPENTER:
            if (!send_entries_.empty() && !scanner_.transmitting()) {
                const SendEntry &entry = send_entries_[send_selection_];
                if (entry.allowed) {
                    scanner_.request_transmit(entry.path);
                    show_message("sending...");
                } else {
                    show_message("tx blocked on this band");
                }
                update_labels();
            }
            return;
        default:
            return;
        }
    }

    if (key == KEY_ESC) {
        if (navigate_home) navigate_home();
        return;
    }
    if (key == KEY_ENTER || key == KEY_KPENTER) {
        if (scanner_ok_) scanner_.toggle_capture(spectrum::bin_frequency(spectrum::kBands[band_], marker_));
        return;
    }
    if (key == KEY_MINUS || key == KEY_EQUAL) {
        scanner_.adjust_threshold(key == KEY_MINUS ? -3 : 3);
        update_labels();
        return;
    }
    // Band and marker stay fixed while a capture is running.
    if (scanner_.capturing()) return;

    switch (key) {
    case KEY_LEFT:
        marker_ = std::max(0, marker_ - 1);
        break;
    case KEY_RIGHT:
        marker_ = std::min(spectrum::kBins - 1, marker_ + 1);
        break;
    case KEY_UP:
        select_band((band_ + spectrum::kBands.size() - 1) % spectrum::kBands.size());
        return;
    case KEY_DOWN:
    case KEY_TAB:
        select_band((band_ + 1) % spectrum::kBands.size());
        return;
    case KEY_1:
        select_band(0);
        return;
    case KEY_2:
        select_band(1);
        return;
    case KEY_3:
        select_band(2);
        return;
    case KEY_R:
        if (scanner_ok_) scanner_.toggle_recording();
        return;
    case KEY_P:
        if (scanner_ok_) enter_send_view();
        return;
    case KEY_SPACE:
        scanner_.set_paused(!scanner_.paused());
        break;
    case KEY_H:
        peak_ = latest_;
        break;
    default:
        return;
    }
    draw_trace();
    update_labels();
}

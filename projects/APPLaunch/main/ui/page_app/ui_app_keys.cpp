/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_keys.hpp"

#include "../app_support.hpp"
#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "input_keys.h"

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <utility>

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
constexpr uint32_t kMessageMs = 2500;
// The scanner deletes an empty capture's files a moment after it reports the
// capture closed, so the list is rebuilt after this delay.
constexpr uint32_t kRescanDelayMs = 250;
constexpr std::size_t kMaxEntries = 32;
constexpr std::size_t kNameColumns = 14;
constexpr std::size_t kHexColumns = 10;

std::string captures_dir()
{
    return app_support::home_dir() + "/spectrum";
}

// "433.920" from the scanner's formatter, shortened to "433.92".
std::string short_mhz(uint32_t hz)
{
    std::string mhz = spectrum::format_mhz(hz);
    if (mhz.size() > 1 && mhz.back() == '0' && mhz.find('.') != std::string::npos) mhz.pop_back();
    return mhz;
}

std::string pad(std::string text, std::size_t width)
{
    if (text.size() < width) text.append(width - text.size(), ' ');
    return text;
}

void set_visible(lv_obj_t *obj, bool on)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == !on) return;
    if (on)
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

} // namespace

UIKeysPage::UIKeysPage() : AppPage()
{
    set_page_title("Keys");
    reclaim_top_bar();
    pixels_.assign(static_cast<std::size_t>(kWaveW) * kWaveH, app_support::rgb565(cp0::theme::kPanel));
    create_ui();

    // The scanner is only used for capture and replay here: paused, it leaves
    // the radio alone between requests.
    scanner_.set_paused(true);
    scanner_.set_auto_capture(false);
    scanner_ok_ = scanner_.start(scanner_error_);
    if (!scanner_ok_) show_message(scanner_error_, true);

    build_entries();
    select(0);

    timer_ = lv_timer_create(timer_cb, kRefreshMs, this);
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

UIKeysPage::~UIKeysPage()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
    scanner_.stop();
}

void UIKeysPage::create_ui()
{
    if (!ui_APP_Container) return;
    cp0::ui::paint_background(ui_APP_Container);
    const cp0::ui::TitleRow row = cp0::ui::title_row(ui_APP_Container, "keys");
    status_ = row.status;

    code_ = cp0::ui::readout(ui_APP_Container, 4, 19, 154, 40, "code");
    te_ = cp0::ui::readout(ui_APP_Container, 162, 19, 154, 40, "te  us");

    cp0::ui::panel(ui_APP_Container, 4, 63, 312, 50);
    canvas_ = lv_canvas_create(ui_APP_Container);
    if (canvas_) {
        lv_canvas_set_buffer(canvas_, pixels_.data(), kWaveW, kWaveH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(canvas_, kWaveX, kWaveY);
    }
    for (lv_obj_t *&label : bit_labels_) {
        label = cp0::ui::label(ui_APP_Container, kWaveX, kBitsY, kColorDim);
        if (label) lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
    info_ = cp0::ui::label(ui_APP_Container, kWaveX, kBitsY, kColorDim);
    if (info_) {
        lv_obj_set_width(info_, kWaveW);
        lv_label_set_long_mode(info_, LV_LABEL_LONG_CLIP);
    }

    cp0::ui::panel(ui_APP_Container, 4, 117, 312, 34);
    for (int i = 0; i < kRows; ++i) {
        row_boxes_[i] = cp0::ui::box(ui_APP_Container, 5, 120 + i * 14, 310, 12, cp0::theme::kSelect);
        if (!row_boxes_[i]) continue;
        row_labels_[i] = cp0::ui::label(row_boxes_[i], 4, 1, kColorText);
        if (row_labels_[i]) {
            lv_obj_set_width(row_labels_[i], 304);
            lv_label_set_long_mode(row_labels_[i], LV_LABEL_LONG_CLIP);
        }
    }
    empty_ = cp0::ui::label(ui_APP_Container, 9, 121, kColorIdle);

    hint_ = cp0::ui::footer(ui_APP_Container);
}

void UIKeysPage::show_message(const std::string &text, bool error)
{
    message_ = cp0::ui::lower(text);
    message_error_ = error;
    message_until_ = lv_tick_get() + kMessageMs;
}

// Newest first, as the Spectrum TX tab lists them.
void UIKeysPage::build_entries()
{
    entries_.clear();
    const std::string directory = captures_dir();
    DIR *dir = opendir(directory.c_str());
    if (!dir) return;
    std::vector<std::pair<long, std::string>> dated;
    while (dirent *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".sub") != 0) continue;
        struct stat info{};
        const long mtime = stat((directory + "/" + name).c_str(), &info) == 0 ? static_cast<long>(info.st_mtime) : 0;
        dated.emplace_back(mtime, name);
    }
    closedir(dir);
    std::sort(dated.begin(), dated.end(), [](const auto &a, const auto &b) {
        return a.first != b.first ? a.first > b.first : a.second > b.second;
    });
    if (dated.size() > kMaxEntries) dated.resize(kMaxEntries);
    for (const auto &item : dated) {
        Entry entry;
        entry.path = directory + "/" + item.second;
        spectrum::Capture capture = spectrum::parse_sub(app_support::read_file(entry.path));
        entry.freq_hz = capture.freq_hz;
        entry.allowed = spectrum::tx_allowed(capture.freq_hz) && !capture.durations.empty();
        entry.label = keys::capture_label(item.second.substr(0, item.second.size() - 4));
        entry.analysis = keys::analyze(capture.durations);
        entry.durations = std::move(capture.durations);
        entries_.push_back(std::move(entry));
    }
}

void UIKeysPage::select(int index)
{
    if (entries_.empty()) {
        selection_ = 0;
        durations_.clear();
        analysis_ = keys::Analysis{};
        zoom_ = 0;
        view_start_us_ = 0;
        render();
        return;
    }
    selection_ = std::clamp(index, 0, static_cast<int>(entries_.size()) - 1);
    const Entry &entry = entries_[selection_];
    durations_ = entry.durations;
    analysis_ = entry.analysis;
    set_zoom(0);
}

void UIKeysPage::show_durations(std::vector<int32_t> durations)
{
    durations_ = std::move(durations);
    analysis_ = keys::analyze(durations_);
    set_zoom(0);
}

uint32_t UIKeysPage::view_span() const
{
    return keys::view_span_us(analysis_, zoom_, kWaveW);
}

void UIKeysPage::set_zoom(int level)
{
    const uint32_t old_span = view_span();
    const int64_t centre = static_cast<int64_t>(view_start_us_) + old_span / 2;
    zoom_ = ((level % keys::kZoomLevels) + keys::kZoomLevels) % keys::kZoomLevels;
    const uint32_t span = view_span();
    // Level 0 frames the decoded packet; other levels zoom about the centre.
    const int64_t start = zoom_ == 0 ? static_cast<int64_t>(analysis_.packet_start_us)
                                     : centre - static_cast<int64_t>(span / 2);
    view_start_us_ = keys::clamp_view_start(start, span, analysis_.total_us);
    render();
}

void UIKeysPage::scroll(int direction)
{
    const uint32_t span = view_span();
    const int64_t step = std::max<int64_t>(span / 4, 1);
    const uint32_t start =
        keys::clamp_view_start(static_cast<int64_t>(view_start_us_) + direction * step, span, analysis_.total_us);
    if (start == view_start_us_) return;
    view_start_us_ = start;
    draw_wave();
}

void UIKeysPage::render()
{
    render_readouts();
    render_list();
    render_status();
    draw_wave();
}

void UIKeysPage::render_readouts()
{
    const keys::Analysis &a = analysis_;
    std::string caption = "code";
    std::string value = "--";
    uint32_t color = kColorIdle;
    if (a.bit_count > 0) {
        caption += "  " + std::to_string(a.bit_count) + " bit" + (a.confident ? "" : "  unsure");
        value = a.hex.size() > kHexColumns ? a.hex.substr(0, kHexColumns - 1) + "+" : a.hex;
        color = a.confident ? kColorText : kColorWarn;
    }
    if (code_.label) cp0::ui::set_text(code_.label, caption.c_str(), kColorDim);
    cp0::ui::set_readout(code_, value, "", color);

    if (scanner_.capturing()) {
        caption = "rssi  trig " + std::to_string(capture_.threshold_dbm);
        if (te_.label) cp0::ui::set_text(te_.label, caption.c_str(), kColorDim);
        cp0::ui::set_readout(te_, std::to_string(capture_.rssi_dbm), "dBm",
                             capture_.rssi_dbm >= capture_.threshold_dbm ? kColorAccent : kColorText);
        return;
    }
    caption = "te  us";
    if (a.packets > 0) caption += "   " + std::to_string(a.repeats) + "/" + std::to_string(a.packets) + " pkts";
    if (te_.label) cp0::ui::set_text(te_.label, caption.c_str(), kColorDim);
    if (a.te_us > 0 && a.packets > 0)
        cp0::ui::set_readout(te_, std::to_string(a.te_us), keys::encoding_label(a.encoding),
                             a.confident ? kColorAccent : kColorWarn);
    else
        cp0::ui::set_readout(te_, "--", "", kColorIdle);
}

void UIKeysPage::render_list()
{
    const int count = static_cast<int>(entries_.size());
    set_visible(empty_, count == 0);
    if (count == 0 && empty_) cp0::ui::set_text(empty_, "no captures yet. r records one", kColorIdle);
    int first = std::clamp(selection_ - kRows + 1, 0, std::max(0, count - kRows));
    if (selection_ < first) first = selection_;
    for (int i = 0; i < kRows; ++i) {
        const int index = first + i;
        const bool used = index < count;
        set_visible(row_boxes_[i], used);
        if (!used || !row_boxes_[i]) continue;
        const Entry &entry = entries_[index];
        const bool on = index == selection_;
        const lv_opa_t opa = on ? LV_OPA_COVER : LV_OPA_TRANSP;
        if (lv_obj_get_style_bg_opa(row_boxes_[i], LV_PART_MAIN) != opa) lv_obj_set_style_bg_opa(row_boxes_[i], opa, 0);
        std::string code = entry.durations.empty() ? "empty" : keys::encoding_label(entry.analysis.encoding);
        if (entry.analysis.bit_count > 0) code += " " + std::to_string(entry.analysis.bit_count);
        std::string text = on ? "> " : "  ";
        text += pad(entry.label.substr(0, kNameColumns), kNameColumns + 1);
        text += pad(short_mhz(entry.freq_hz), 9);
        text += pad(code, 9);
        if (entry.analysis.repeats > 0) text += "x" + std::to_string(entry.analysis.repeats);
        const uint32_t color = !entry.allowed ? kColorRed : on ? kColorAccent : kColorText;
        cp0::ui::set_text(row_labels_[i], text.c_str(), color);
    }
}

void UIKeysPage::render_status()
{
    std::string text;
    uint32_t color = kColorIdle;
    if (!scanner_ok_) {
        text = "no radio";
        color = kColorRed;
    } else if (scanner_.capturing()) {
        text = "rec " + short_mhz(capture_.freq_hz) + "  " + std::to_string(capture_.packets) + " pkt";
        color = kColorRed;
    } else if (scanner_.transmitting()) {
        text = "sending";
        color = kColorAccent;
    } else if (!entries_.empty()) {
        const Entry &entry = entries_[selection_];
        text = short_mhz(entry.freq_hz) + "  ook";
        color = entry.allowed ? kColorAccent : kColorRed;
    } else {
        text = "no captures";
    }
    cp0::ui::set_text(status_, text.c_str(), color);

    if (!hint_) return;
    if (message_until_ != 0 && !message_.empty())
        cp0::ui::set_footer_message(hint_, message_.c_str(), message_error_ ? kColorRed : kColorAccent);
    else
        cp0::ui::set_footer(hint_, {{"r", scanner_.capturing() ? "stop" : "record"}, {"ent", "send"},
                                    {"< >", "scroll"}, {"z", "zoom"}, {"up/dn", "pick"}});
}

void UIKeysPage::draw_wave()
{
    if (!canvas_) return;
    const uint16_t bg = app_support::rgb565(cp0::theme::kPanel);
    const uint16_t line = app_support::rgb565(kColorAccent);
    const uint16_t fill = app_support::rgb565(kColorFill);
    const uint16_t outside = app_support::rgb565(cp0::theme::kLine);
    const int top = 2;
    const int bottom = kWaveH - 3;
    const uint32_t span = view_span();
    const std::vector<uint8_t> columns = keys::waveform_columns(durations_, view_start_us_, span, kWaveW);
    std::fill(pixels_.begin(), pixels_.end(), bg);
    auto put = [this](int x, int y, uint16_t color) { pixels_[static_cast<std::size_t>(y) * kWaveW + x] = color; };
    uint8_t previous = 0;
    for (int x = 0; x < kWaveW; ++x) {
        const uint8_t level = columns[static_cast<std::size_t>(x)];
        if (level == 0) {
            if (x % 3 == 0) put(x, bottom, outside);
        } else if (level == (keys::kColumnHigh | keys::kColumnLow) || (previous != 0 && previous != level)) {
            // An edge inside this column, or exactly on its left side.
            for (int y = top; y <= bottom; ++y) put(x, y, line);
        } else if (level == keys::kColumnHigh) {
            put(x, top, line);
            for (int y = top + 1; y <= bottom; ++y) put(x, y, fill);
        } else {
            put(x, bottom, line);
        }
        previous = level == (keys::kColumnHigh | keys::kColumnLow) ? 0 : level;
    }
    lv_obj_invalidate(canvas_);

    // Bit values under the trace when a glyph fits under every bit.
    const keys::Analysis &a = analysis_;
    int used = 0;
    if (a.bit_count > 0 && a.bit_edges_us.size() == static_cast<std::size_t>(a.bit_count) + 1) {
        const int width = keys::column_of(a.bit_edges_us.back(), view_start_us_, span, kWaveW) -
                          keys::column_of(a.bit_edges_us.front(), view_start_us_, span, kWaveW);
        const int padding = (4 - a.bit_count % 4) % 4;
        for (int bit = 0; width >= a.bit_count * kGlyphW && bit < a.bit_count && used < kBitLabels; ++bit) {
            const int x0 = keys::column_of(a.bit_edges_us[static_cast<std::size_t>(bit)], view_start_us_, span, kWaveW);
            const int x1 =
                keys::column_of(a.bit_edges_us[static_cast<std::size_t>(bit) + 1], view_start_us_, span, kWaveW);
            const int x = (x0 + x1) / 2 - kGlyphW / 2;
            if (x < 0 || x + kGlyphW > kWaveW) continue;
            lv_obj_t *label = bit_labels_[static_cast<std::size_t>(used++)];
            if (!label) continue;
            // Alternate shades per hex digit.
            const bool even = ((bit + padding) / 4) % 2 == 0;
            cp0::ui::set_text(label, a.bits[static_cast<std::size_t>(bit)] == '1' ? "1" : "0",
                              even ? kColorDim : kColorIdle);
            if (lv_obj_get_x(label) != kWaveX + x) lv_obj_set_x(label, kWaveX + x);
            set_visible(label, true);
        }
    }
    for (int i = used; i < kBitLabels; ++i) set_visible(bit_labels_[static_cast<std::size_t>(i)], false);

    set_visible(info_, used == 0);
    if (used == 0 && info_) {
        std::string text;
        if (durations_.empty())
            text = scanner_.capturing() ? "waiting for a signal" : "";
        else
            text = "at " + keys::format_duration(view_start_us_) + "  span " + keys::format_duration(span) + "  of " +
                   keys::format_duration(analysis_.total_us);
        cp0::ui::set_text(info_, text.c_str(), kColorDim);
    }
}

void UIKeysPage::timer_cb(lv_timer_t *timer) noexcept
{
    auto *page = static_cast<UIKeysPage *>(lv_timer_get_user_data(timer));
    if (!page) return;
    try {
        page->on_timer();
    } catch (...) {
    }
}

void UIKeysPage::on_timer()
{
    const std::string message = scanner_.take_message();
    if (!message.empty()) show_message(message);
    if (message_until_ != 0 && lv_tick_get() >= message_until_) message_until_ = 0;

    const bool capturing = scanner_.capturing();
    if (capturing || was_capturing_) capture_ = scanner_.capture_status();
    if (capturing && !was_capturing_) {
        live_sequence_ = 0;
        show_durations({});
    }
    if (capturing && capture_.sequence != live_sequence_) {
        live_sequence_ = capture_.sequence;
        show_durations(capture_.last);
    }
    if (!capturing && was_capturing_) rescan_at_ = lv_tick_get() + kRescanDelayMs;
    was_capturing_ = capturing;
    if (rescan_at_ != 0 && lv_tick_get() >= rescan_at_) {
        rescan_at_ = 0;
        build_entries();
        select(0);
    }

    render_readouts();
    render_status();
}

void UIKeysPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UIKeysPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item) return;
    // Z is the zoom key here, so its navigation alias (left) is not used.
    const uint32_t semantic = item->semantic_key ? item->semantic_key : item->key_code;
    const uint32_t key = item->key_code == KEY_Z ? static_cast<uint32_t>(KEY_Z) : semantic;
    try {
        page->handle_key(key, item->key_state == KBD_KEY_REPEATED);
    } catch (...) {
    }
}

void UIKeysPage::handle_key(uint32_t key, bool repeat)
{
    if (repeat && key != KEY_LEFT && key != KEY_RIGHT && key != KEY_UP && key != KEY_DOWN) return;
    const bool capturing = scanner_.capturing();
    switch (key) {
    case KEY_ESC:
        if (navigate_home) navigate_home();
        return;
    case KEY_UP:
    case KEY_DOWN: {
        if (capturing || entries_.empty()) return;
        const int next = std::clamp(selection_ + (key == KEY_DOWN ? 1 : -1), 0, static_cast<int>(entries_.size()) - 1);
        if (next != selection_) select(next);
        return;
    }
    case KEY_LEFT:
    case KEY_RIGHT:
        scroll(key == KEY_RIGHT ? 1 : -1);
        return;
    case KEY_Z:
        set_zoom(zoom_ + 1);
        return;
    case KEY_ENTER:
    case KEY_KPENTER:
        if (!scanner_ok_) {
            show_message(scanner_error_, true);
        } else if (capturing) {
            show_message("stop the capture first", true);
        } else if (entries_.empty() || scanner_.transmitting()) {
            return;
        } else if (!entries_[selection_].allowed) {
            show_message("cannot tx this capture", true);
        } else {
            scanner_.request_transmit(entries_[selection_].path);
            show_message("sending " + short_mhz(entries_[selection_].freq_hz));
        }
        render_status();
        return;
    case KEY_R:
        if (!scanner_ok_) {
            show_message(scanner_error_, true);
        } else if (scanner_.transmitting()) {
            return;
        } else {
            const uint32_t selected = entries_.empty() ? 0 : entries_[selection_].freq_hz;
            const uint32_t hz = spectrum::tx_allowed(selected) ? selected : kDefaultHz;
            scanner_.toggle_capture(hz);
            if (!capturing) show_message("listening on " + short_mhz(hz));
        }
        render_status();
        return;
    default:
        return;
    }
}

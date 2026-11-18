/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_adsb.hpp"

#include "../app_support.hpp"
#include "../model/sdr_model.hpp"
#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "input_keys.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace {

constexpr uint32_t kRefreshMs = 100;
constexpr uint32_t kMessageMs = 3000;
constexpr uint32_t kPopupMs = 5000;
constexpr uint32_t kDetectEveryMs = 3000;
constexpr uint32_t kDetectTimeoutMs = 10000;
constexpr uint32_t kLocationEveryMs = 5000;
constexpr long kOutputLimit = 1 << 20;
constexpr double kPi = 3.14159265358979323846;
constexpr double kAdsbHz = 1090e6;
constexpr int kRangesNm[] = {20, 40, 80, 160};
constexpr int kRangeCount = 4;
constexpr const char *kDetectDone = "cp0-detect-done";
constexpr const char *kExitMark = "#exit";
constexpr const char *kRangeConfigKey = "adsb_range";

double now_seconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int config_range()
{
    int value = 1;
    cp0_signal_config_api({"GetInt", kRangeConfigKey, "1"}, [&](int code, std::string data) {
        if (code == 0) value = std::atoi(data.c_str());
    });
    return std::clamp(value, 0, kRangeCount - 1);
}

void set_config_range(int index)
{
    cp0_signal_config_api({"SetInt", kRangeConfigKey, std::to_string(index)}, nullptr);
    cp0_signal_config_api({"Save"}, nullptr);
}

// New bytes appended to a file since `offset`.
std::string tail_file(const std::string &path, long &offset)
{
    std::FILE *file = std::fopen(path.c_str(), "r");
    if (!file) return {};
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    if (size < offset) offset = 0; // file was truncated
    std::string chunk;
    if (size > offset) {
        std::fseek(file, offset, SEEK_SET);
        chunk.resize(static_cast<std::size_t>(size - offset));
        const std::size_t read = std::fread(&chunk[0], 1, chunk.size(), file);
        chunk.resize(read);
        offset += static_cast<long>(read);
    }
    std::fclose(file);
    return chunk;
}

void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == hidden) return;
    if (hidden)
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

} // namespace

UIAdsbPage::UIAdsbPage() : AppPage()
{
    set_page_title("ADSB");
    reclaim_top_bar();
    range_index_ = config_range();
    detect_path_ = app_support::runtime_dir() + "/cp0-adsb-detect";
    out_path_ = app_support::runtime_dir() + "/cp0-adsb-out";
    pixels_.assign(static_cast<std::size_t>(kCanvasW) * kCanvasH, app_support::rgb565(cp0::theme::kPanel));
    tools_ok_ = app_support::tool_present("rtl_adsb") && app_support::tool_present("rtl_test");
    load_location();
    create_ui();
    show_popup(true);
    render();
    draw_scope();
    if (tools_ok_) start_detect();
    timer_ = lv_timer_create(timer_cb, kRefreshMs, this);
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

UIAdsbPage::~UIAdsbPage()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
    stop_receiver();
    app_support::stop(detect_pid_);
    unlink(detect_path_.c_str());
    unlink(out_path_.c_str());
}

void UIAdsbPage::create_ui()
{
    if (!ui_APP_Container) return;
    cp0::ui::paint_background(ui_APP_Container);
    const cp0::ui::TitleRow row = cp0::ui::title_row(ui_APP_Container, "adsb");
    status_ = row.status;

    cp0::ui::panel(ui_APP_Container, 4, 19, 134, 132);
    canvas_ = lv_canvas_create(ui_APP_Container);
    if (canvas_) {
        lv_canvas_set_buffer(canvas_, pixels_.data(), kCanvasW, kCanvasH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(canvas_, kCanvasX, kCanvasY);
    }
    ring_half_ = cp0::ui::label(ui_APP_Container, kCanvasX + kCentreX + 2, kCanvasY + kCentreY - kRadius / 2 + 1,
                                cp0::theme::kBracket);
    ring_full_ = cp0::ui::label(ui_APP_Container, kCanvasX + kCentreX + 2, kCanvasY + kCentreY - kRadius + 1,
                                cp0::theme::kBracket);
    tag_ = cp0::ui::label(ui_APP_Container, 0, 0, cp0::theme::kText);
    set_hidden(tag_, true);

    const cp0::ui::Panel list = cp0::ui::panel(ui_APP_Container, 142, 19, 174, 132);
    if (list.box) {
        lv_obj_t *header = cp0::ui::label(list.box, 5, 3, cp0::theme::kDim);
        if (header) lv_label_set_text(header, "flight    fl   nm  brg");
        cp0::ui::dotted_rule(list.box, 4, 15, 166);
        select_bar_ = cp0::ui::box(list.box, 1, 18, 172, 12, cp0::theme::kSelect);
        set_hidden(select_bar_, true);
        for (int i = 0; i < kRows; ++i) {
            row_labels_[i] = cp0::ui::label(list.box, 5, 19 + i * 13, cp0::theme::kText);
            if (row_labels_[i]) {
                lv_obj_set_width(row_labels_[i], 166);
                lv_label_set_long_mode(row_labels_[i], LV_LABEL_LONG_CLIP);
            }
        }
        empty_ = cp0::ui::label(list.box, 5, 22, cp0::theme::kIdle);
        if (empty_) {
            lv_obj_set_width(empty_, 164);
            lv_label_set_long_mode(empty_, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_line_space(empty_, 2, 0);
        }
    }
    hint_ = cp0::ui::footer(ui_APP_Container);

    popup_ = cp0::ui::panel(ui_APP_Container, 47, 61, 226, 48);
    if (popup_.box) {
        cp0::ui::set_panel_focus(popup_, true);
        char line[48];
        std::snprintf(line, sizeof(line), "antenna: %.0f mhz", kAdsbHz / 1e6);
        lv_obj_t *first = cp0::ui::label(popup_.box, 8, 5, cp0::theme::kAccent);
        if (first) lv_label_set_text(first, line);
        std::snprintf(line, sizeof(line), "vertical, %.1f cm quarter wave", location::quarter_wave_cm(kAdsbHz));
        lv_obj_t *second = cp0::ui::label(popup_.box, 8, 18, cp0::theme::kText);
        if (second) lv_label_set_text(second, line);
        lv_obj_t *third = cp0::ui::label(popup_.box, 8, 31, cp0::theme::kText);
        if (third) lv_label_set_text(third, "clear view of the sky");
        set_hidden(popup_.box, true);
    }
}

void UIAdsbPage::show_message(const std::string &text)
{
    message_ = text;
    message_until_ = lv_tick_get() + kMessageMs;
}

void UIAdsbPage::show_popup(bool on)
{
    popup_until_ = on ? lv_tick_get() + kPopupMs : 0;
    set_hidden(popup_.box, !on);
}

void UIAdsbPage::load_location()
{
    last_location_ = lv_tick_get();
    observer_ = location::parse(app_support::read_file(location::file_path(app_support::home_dir())));
}

void UIAdsbPage::start_detect()
{
    if (detect_pid_ > 0) return;
    last_detect_ = lv_tick_get();
    unlink(detect_path_.c_str());
    // The marker is written after rtl_test has exited and released the
    // dongle, so the receiver never starts while the probe still holds it.
    detect_pid_ = app_support::spawn("( " + sdr::detect_cmd() + "; echo " + kDetectDone + " ) > '" + detect_path_ +
                                     "' 2>&1");
}

void UIAdsbPage::poll_detect()
{
    const std::string output = app_support::read_file(detect_path_);
    const bool done = output.find(kDetectDone) != std::string::npos;
    if (!done && lv_tick_get() - last_detect_ < kDetectTimeoutMs) return;
    app_support::stop(detect_pid_);
    last_detect_ = lv_tick_get();
    if (done && sdr::parse_detect(output).present) {
        present_ = true;
        start_receiver();
    }
    render();
}

void UIAdsbPage::start_receiver()
{
    stop_receiver();
    out_offset_ = 0;
    pending_.clear();
    if (std::FILE *file = std::fopen(out_path_.c_str(), "w")) std::fclose(file);
    // rtl_adsb prints frames to stdout when no file is named (unverified
    // against its source). stdbuf asks for line buffering so frames reach the
    // file as they are decoded. Output goes to a regular file, so the tool is
    // never blocked on a pipe when it is signalled to stop. The marker line
    // tells the page the tool exited on its own, e.g. the dongle was pulled.
    const std::string tool = app_support::tool_present("stdbuf") ? "stdbuf -oL rtl_adsb" : "rtl_adsb";
    proc_pid_ = app_support::spawn("( " + tool + " 2>/dev/null; echo '" + kExitMark + "' ) >> '" + out_path_ + "'");
    if (proc_pid_ <= 0) {
        present_ = false;
        show_message("cannot start rtl_adsb");
    }
}

void UIAdsbPage::stop_receiver()
{
    app_support::stop(proc_pid_);
}

void UIAdsbPage::read_frames()
{
    pending_ += tail_file(out_path_, out_offset_);
    const double now = now_seconds();
    std::size_t start = 0;
    bool exited = false;
    while (true) {
        const std::size_t nl = pending_.find('\n', start);
        if (nl == std::string::npos) break;
        const std::string line = pending_.substr(start, nl - start);
        start = nl + 1;
        if (line.compare(0, 5, kExitMark) == 0)
            exited = true;
        else
            table_.feed(line, now);
    }
    pending_.erase(0, start);
    if (pending_.size() > 256) pending_.clear();

    if (exited) {
        stop_receiver();
        present_ = false;
        last_detect_ = lv_tick_get();
        return;
    }
    // The tool appends, so emptying the file under it keeps the scratch file
    // small; frames written between the read above and here are lost.
    if (out_offset_ > kOutputLimit && pending_.empty() && truncate(out_path_.c_str(), 0) == 0) out_offset_ = 0;
}

int UIAdsbPage::range_nm() const
{
    return kRangesNm[std::clamp(range_index_, 0, kRangeCount - 1)];
}

void UIAdsbPage::rebuild_rows()
{
    // Without an observer the scope centres on the aircraft themselves.
    location::Position centre = observer_;
    if (!centre.valid) {
        double lat = 0.0;
        double x = 0.0;
        double y = 0.0;
        int count = 0;
        for (const adsb::Aircraft &a : table_.aircraft()) {
            if (!a.has_position) continue;
            lat += a.lat;
            x += std::cos(a.lon * kPi / 180.0);
            y += std::sin(a.lon * kPi / 180.0);
            ++count;
        }
        if (count > 0) {
            centre.valid = true;
            centre.lat = lat / count;
            centre.lon = std::atan2(y, x) * 180.0 / kPi;
        }
    }

    rows_.clear();
    for (const adsb::Aircraft &a : table_.aircraft()) {
        Row row;
        row.icao = a.icao;
        row.label = adsb::label(a);
        row.has_track = a.has_velocity;
        row.track_deg = a.track_deg;
        if (a.has_position && centre.valid) {
            location::Position where;
            where.valid = true;
            where.lat = a.lat;
            where.lon = a.lon;
            row.has_position = true;
            row.distance_nm = location::distance_nm(centre, where);
            row.bearing_deg = location::bearing_deg(centre, where);
        }
        char text[40];
        if (row.has_position)
            std::snprintf(text, sizeof(text), "%-8.8s%4s%5d%5d", row.label.c_str(), adsb::format_flight_level(a).c_str(),
                          static_cast<int>(std::min(std::lround(row.distance_nm), 9999L)),
                          static_cast<int>(std::lround(row.bearing_deg)) % 360);
        else
            std::snprintf(text, sizeof(text), "%-8.8s%4s%5s%5s", row.label.c_str(), adsb::format_flight_level(a).c_str(),
                          "--", "--");
        row.text = cp0::ui::lower(text);
        rows_.push_back(row);
    }
    std::sort(rows_.begin(), rows_.end(), [](const Row &a, const Row &b) {
        if (a.has_position != b.has_position) return a.has_position;
        if (a.has_position && a.distance_nm != b.distance_nm) return a.distance_nm < b.distance_nm;
        return a.icao < b.icao;
    });

    const bool found = has_selection_ && std::any_of(rows_.begin(), rows_.end(),
                                                     [this](const Row &r) { return r.icao == selected_icao_; });
    if (!found) {
        has_selection_ = !rows_.empty();
        if (has_selection_) selected_icao_ = rows_.front().icao;
    }
}

void UIAdsbPage::move_selection(int delta)
{
    if (rows_.empty()) return;
    int index = 0;
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].icao == selected_icao_) index = static_cast<int>(i);
    index = std::clamp(index + delta, 0, static_cast<int>(rows_.size()) - 1);
    selected_icao_ = rows_[static_cast<std::size_t>(index)].icao;
    has_selection_ = true;
    render();
    draw_scope();
}

void UIAdsbPage::draw_scope()
{
    if (!canvas_) return;
    const uint16_t bg = app_support::rgb565(cp0::theme::kPanel);
    const uint16_t grid = app_support::rgb565(cp0::theme::kLine);
    const uint16_t accent = app_support::rgb565(cp0::theme::kAccent);
    const uint16_t trail = app_support::rgb565(cp0::theme::kBracket);
    const uint16_t text = app_support::rgb565(cp0::theme::kText);

    auto put = [this](int x, int y, uint16_t color) {
        if (x < 0 || y < 0 || x >= kCanvasW || y >= kCanvasH) return;
        pixels_[static_cast<std::size_t>(y) * kCanvasW + x] = color;
    };
    auto line = [&put](double x0, double y0, double x1, double y1, uint16_t color) {
        const int n = std::max(1, static_cast<int>(std::max(std::fabs(x1 - x0), std::fabs(y1 - y0))));
        for (int i = 0; i <= n; ++i)
            put(static_cast<int>(std::lround(x0 + (x1 - x0) * i / n)), static_cast<int>(std::lround(y0 + (y1 - y0) * i / n)),
                color);
    };
    auto circle = [&put](int cx, int cy, int r, uint16_t color, int gap) {
        const int n = static_cast<int>(std::ceil(r * 6.4));
        for (int i = 0; i < n; i += gap)
            put(static_cast<int>(std::lround(cx + r * std::cos(i * 2.0 * kPi / n))),
                static_cast<int>(std::lround(cy + r * std::sin(i * 2.0 * kPi / n))), color);
    };

    std::fill(pixels_.begin(), pixels_.end(), bg);
    for (int r = kRadius / 4; r <= kRadius; r += kRadius / 4) circle(kCentreX, kCentreY, r, grid, 1);
    for (int d = -kRadius; d <= kRadius; d += 2) {
        put(kCentreX + d, kCentreY, grid);
        put(kCentreX, kCentreY + d, grid);
    }

    if (present_ && proc_pid_ > 0) {
        for (int k = 5; k >= 0; --k) {
            const double angle = sweep_ - k * 0.05;
            line(kCentreX, kCentreY, kCentreX + kRadius * std::sin(angle), kCentreY - kRadius * std::cos(angle),
                 k == 0 ? accent : k < 3 ? trail : grid);
        }
    }

    bool tagged = false;
    for (const Row &row : rows_) {
        if (!row.has_position) continue;
        const double r = row.distance_nm / range_nm() * kRadius;
        if (r > kRadius) continue;
        const double bearing = row.bearing_deg * kPi / 180.0;
        const int x = static_cast<int>(std::lround(kCentreX + r * std::sin(bearing)));
        const int y = static_cast<int>(std::lround(kCentreY - r * std::cos(bearing)));
        const bool selected = has_selection_ && row.icao == selected_icao_;
        const uint16_t color = selected ? text : accent;
        if (row.has_track) {
            const double heading = row.track_deg * kPi / 180.0;
            line(x, y, x + 5.0 * std::sin(heading), y - 5.0 * std::cos(heading), color);
        }
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) put(x + dx, y + dy, color);
        if (selected) {
            circle(x, y, 6, text, 2);
            if (tag_) {
                const std::string name = cp0::ui::lower(row.label);
                const int width = static_cast<int>(name.size()) * 7;
                const int tx = x + 8 + width > kCanvasW ? x - 8 - width : x + 8;
                const int ty = std::clamp(y - 11, 0, kCanvasH - 12);
                cp0::ui::set_text(tag_, name.c_str(), cp0::theme::kText);
                if (lv_obj_get_x(tag_) != kCanvasX + tx || lv_obj_get_y(tag_) != kCanvasY + ty)
                    lv_obj_set_pos(tag_, kCanvasX + tx, kCanvasY + ty);
                tagged = true;
            }
        }
    }
    set_hidden(tag_, !tagged);
    lv_obj_invalidate(canvas_);
}

void UIAdsbPage::render()
{
    const bool receiving = present_ && proc_pid_ > 0;
    if (status_) {
        std::string text;
        uint32_t color = cp0::theme::kIdle;
        if (lv_tick_get() < message_until_) {
            text = message_;
            color = cp0::theme::kText;
        } else if (!tools_ok_) {
            text = "not installed";
            color = cp0::theme::kDanger;
        } else if (!receiving) {
            text = "no dongle";
        } else {
            text = (observer_.valid ? "1090  " : "no location  ") + std::to_string(rows_.size()) + " ac";
            color = observer_.valid ? cp0::theme::kAccent : cp0::theme::kWarn;
        }
        cp0::ui::set_text(status_, text.c_str(), color);
    }

    cp0::ui::set_text(ring_half_, std::to_string(range_nm() / 2).c_str(), cp0::theme::kBracket);
    cp0::ui::set_text(ring_full_, std::to_string(range_nm()).c_str(), cp0::theme::kBracket);

    int selected = -1;
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (has_selection_ && rows_[i].icao == selected_icao_) selected = static_cast<int>(i);
    const int count = static_cast<int>(rows_.size());
    scroll_ = std::clamp(scroll_, 0, std::max(0, count - kRows));
    if (selected >= 0) {
        if (selected < scroll_) scroll_ = selected;
        if (selected >= scroll_ + kRows) scroll_ = selected - kRows + 1;
    }
    for (int i = 0; i < kRows; ++i) {
        const int index = scroll_ + i;
        const bool used = index < count;
        cp0::ui::set_text(row_labels_[i], used ? rows_[static_cast<std::size_t>(index)].text.c_str() : "",
                          index == selected ? cp0::theme::kAccent : cp0::theme::kText);
    }
    if (select_bar_) {
        const bool shown = selected >= 0;
        set_hidden(select_bar_, !shown);
        const int y = 18 + (selected - scroll_) * 13;
        if (shown && lv_obj_get_y(select_bar_) != y) lv_obj_set_y(select_bar_, y);
    }

    if (empty_) {
        std::string text;
        if (!tools_ok_) {
            text = "rtl_adsb not installed\nrun: sudo apt install rtl-sdr";
        } else if (!receiving) {
            text = "no sdr\nplug in the rtl-sdr dongle";
        } else if (rows_.empty()) {
            text = "listening on 1090 mhz";
            if (!observer_.valid) text += std::string("\n\nno location. set it:\n") + location::set_hint();
        }
        cp0::ui::set_text(empty_, rows_.empty() ? text.c_str() : "", cp0::theme::kIdle);
    }

    cp0::ui::set_footer(hint_, {{"up/dn", "select"}, {"+ -", "range"}, {"a", "antenna"}});
}

void UIAdsbPage::timer_cb(lv_timer_t *timer) noexcept
{
    auto *page = static_cast<UIAdsbPage *>(lv_timer_get_user_data(timer));
    if (!page) return;
    try {
        page->on_timer();
    } catch (...) {
    }
}

void UIAdsbPage::on_timer()
{
    const uint32_t tick = lv_tick_get();
    if (popup_until_ != 0 && tick >= popup_until_) show_popup(false);
    if (message_until_ != 0 && tick >= message_until_) message_until_ = 0;
    if (tick - last_location_ >= kLocationEveryMs) load_location();

    if (tools_ok_ && !present_ && detect_pid_ <= 0 && tick - last_detect_ >= kDetectEveryMs) start_detect();
    if (detect_pid_ > 0) poll_detect();
    if (present_ && proc_pid_ > 0) read_frames();

    table_.age(now_seconds());
    rebuild_rows();
    sweep_ = std::fmod(sweep_ + 0.11, 2.0 * kPi);
    render();
    draw_scope();
}

void UIAdsbPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UIAdsbPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item) return;
    const uint32_t key = item->semantic_key ? item->semantic_key : item->key_code;
    try {
        page->handle_key(key, item->key_state == KBD_KEY_REPEATED);
    } catch (...) {
    }
}

void UIAdsbPage::handle_key(uint32_t key, bool repeat)
{
    if (key == KEY_ESC) {
        if (navigate_home) navigate_home();
        return;
    }
    if (popup_until_ != 0) {
        // The key that dismisses the hint does nothing else.
        if (!repeat) show_popup(false);
        return;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        move_selection(key == KEY_DOWN ? 1 : -1);
        return;
    }
    if (repeat) return;
    if (key == KEY_A) {
        show_popup(true);
    } else if (key == KEY_EQUAL || key == KEY_MINUS) {
        const int next = std::clamp(range_index_ + (key == KEY_EQUAL ? 1 : -1), 0, kRangeCount - 1);
        if (next != range_index_) {
            range_index_ = next;
            set_config_range(range_index_);
        }
        show_message("range " + std::to_string(range_nm()) + " nm");
        render();
        draw_scope();
    }
}

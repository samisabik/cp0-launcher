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
constexpr uint32_t kColorRed = 0xFF5F56;
constexpr uint32_t kColorAmber = 0xE0A84E;
constexpr const char *kBandConfigKey = "spectrum_band";
constexpr uint32_t kRefreshMs = 50;
constexpr uint32_t kMessageMs = 2500;
constexpr int kFinderBarW = 120;
constexpr int kTabW = 160;

int config_band()
{
    int value = 0;
    cp0_signal_config_api({"GetInt", kBandConfigKey, "0"}, [&](int code, std::string data) {
        if (code == 0) value = std::atoi(data.c_str());
    });
    return value;
}

void set_config_band(int value)
{
    cp0_signal_config_api({"SetInt", kBandConfigKey, std::to_string(value)}, nullptr);
    cp0_signal_config_api({"Save"}, nullptr);
}

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

std::string spectrum_dir()
{
    const char *home = std::getenv("HOME");
    return std::string(home && home[0] ? home : "/tmp") + "/spectrum";
}

std::string trim_mhz(uint32_t hz)
{
    std::string mhz = spectrum::format_mhz(hz);
    return mhz.substr(0, mhz.size() - 1); // drop the last digit for a compact label
}

std::string age_text(int64_t ms)
{
    const int64_t seconds = ms / 1000;
    if (seconds < 60) return std::to_string(seconds) + "s";
    if (seconds < 3600) return std::to_string(seconds / 60) + "m";
    return std::to_string(seconds / 3600) + "h";
}

// Grey while weak, amber as it nears the trigger, green once it crosses,
// white when it maxes out.
uint32_t strength_color(int rssi_dbm, int threshold_dbm)
{
    if (rssi_dbm < threshold_dbm - 12) return kColorIdle;
    if (rssi_dbm < threshold_dbm) return kColorAmber;
    if (rssi_dbm < threshold_dbm + 15) return kColorAccent;
    return kColorText;
}

} // namespace

UISpectrumPage::UISpectrumPage() : AppPage()
{
    set_page_title("Spectrum");
    reclaim_top_bar();
    band_ = static_cast<std::size_t>(std::clamp(config_band(), 0, static_cast<int>(spectrum::kBands.size()) - 1));

    if (ui_APP_Container) {
        lv_obj_set_style_bg_color(ui_APP_Container, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(ui_APP_Container, LV_OPA_COVER, 0);
    }
    create_tabbar();
    create_rx();
    create_tx();
    hint_label_ = mk_label(ui_APP_Container, 4, kHintY, 11, kColorDim);
    if (hint_label_) {
        lv_obj_set_width(hint_label_, 312);
        lv_obj_set_style_text_align(hint_label_, LV_TEXT_ALIGN_RIGHT, 0);
    }

    scanner_.set_band(band_);
    scanner_.set_mode(SpectrumScanner::Mode::Finder);
    std::string error;
    scanner_ok_ = scanner_.start(error);
    if (!scanner_ok_) show_message(error);

    select_tab(Tab::Rx);

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
}

lv_obj_t *UISpectrumPage::mk_label(lv_obj_t *parent, int x, int y, int px, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent ? parent : ui_APP_Container);
    if (!label) return nullptr;
    lv_obj_set_style_text_font(label, launcher_fonts().get_chrome(px), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    lv_label_set_text(label, "");
    return label;
}

void UISpectrumPage::create_tabbar()
{
    if (!ui_APP_Container) return;
    // Highlight behind the active tab; created first so labels sit on top.
    tab_hl_ = lv_obj_create(ui_APP_Container);
    if (tab_hl_) {
        lv_obj_remove_style_all(tab_hl_);
        lv_obj_set_size(tab_hl_, kTabW, kTabH);
        lv_obj_set_pos(tab_hl_, 0, kTabTop);
        lv_obj_set_style_bg_color(tab_hl_, lv_color_hex(kColorAccent), 0);
        lv_obj_set_style_bg_opa(tab_hl_, LV_OPA_COVER, 0);
    }
    static const char *const kNames[] = {"RX", "TX"};
    for (int i = 0; i < 2; ++i) {
        tab_labels_[i] = mk_label(ui_APP_Container, 0, kTabTop + 3, 12, kColorIdle);
        if (tab_labels_[i]) {
            lv_obj_set_pos(tab_labels_[i], i * kTabW, kTabTop + 3);
            lv_obj_set_width(tab_labels_[i], kTabW);
            lv_obj_set_style_text_align(tab_labels_[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text(tab_labels_[i], kNames[i]);
        }
    }
    // Hairline under the tab row.
    lv_obj_t *line = lv_obj_create(ui_APP_Container);
    if (line) {
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, kWidth, 1);
        lv_obj_set_pos(line, 0, kTabTop + kTabH);
        lv_obj_set_style_bg_color(line, lv_color_hex(kColorGrid), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    }
}

void UISpectrumPage::create_rx()
{
    rx_panel_ = lv_obj_create(ui_APP_Container);
    if (!rx_panel_) return;
    lv_obj_remove_style_all(rx_panel_);
    lv_obj_clear_flag(rx_panel_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_pos(rx_panel_, 0, kBodyTop);
    lv_obj_set_size(rx_panel_, kWidth, kBodyH);

    for (std::size_t i = 0; i < spectrum::kFinderChannels.size(); ++i) {
        const int y = static_cast<int>(i) * 13;
        finder_freq_[i] = mk_label(rx_panel_, 4, y, 11, kColorIdle);
        lv_obj_t *track = lv_obj_create(rx_panel_);
        if (track) {
            lv_obj_remove_style_all(track);
            lv_obj_set_pos(track, 52, y + 4);
            lv_obj_set_size(track, kFinderBarW, 6);
            lv_obj_set_style_bg_color(track, lv_color_hex(kColorGrid), 0);
            lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
            finder_bar_[i] = lv_obj_create(track);
            if (finder_bar_[i]) {
                lv_obj_remove_style_all(finder_bar_[i]);
                lv_obj_set_size(finder_bar_[i], 0, 6);
                lv_obj_set_style_bg_color(finder_bar_[i], lv_color_hex(kColorIdle), 0);
                lv_obj_set_style_bg_opa(finder_bar_[i], LV_OPA_COVER, 0);
            }
        }
        finder_value_[i] = mk_label(rx_panel_, 52 + kFinderBarW + 6, y, 11, kColorDim);
    }
    hit_title_ = mk_label(rx_panel_, 216, 2, 11, kColorDim);
    if (hit_title_) lv_label_set_text(hit_title_, "hit");
    hit_freq_ = mk_label(rx_panel_, 216, 14, 22, kColorText);
    hit_detail_ = mk_label(rx_panel_, 216, 40, 11, kColorAccent);
    hit_history_ = mk_label(rx_panel_, 216, 58, 11, kColorDim);
    if (hit_history_) lv_obj_set_style_text_line_space(hit_history_, 2, 0);
    rx_status_ = mk_label(rx_panel_, 216, 110, 11, kColorDim);
}

void UISpectrumPage::create_tx()
{
    tx_panel_ = lv_obj_create(ui_APP_Container);
    if (!tx_panel_) return;
    lv_obj_remove_style_all(tx_panel_);
    lv_obj_clear_flag(tx_panel_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_pos(tx_panel_, 0, kBodyTop);
    lv_obj_set_size(tx_panel_, kWidth, kBodyH);
    lv_obj_add_flag(tx_panel_, LV_OBJ_FLAG_HIDDEN);

    tx_list_ = mk_label(tx_panel_, 4, 2, 11, kColorText);
    if (tx_list_) {
        lv_obj_set_width(tx_list_, 206);
        lv_label_set_long_mode(tx_list_, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_line_space(tx_list_, 2, 0);
    }
    tx_detail_ = mk_label(tx_panel_, 216, 14, 22, kColorText);
}

void UISpectrumPage::select_tab(Tab tab)
{
    tab_ = tab;
    const int index = tab == Tab::Rx ? 0 : 1;
    if (tab_hl_) lv_obj_set_pos(tab_hl_, index * kTabW, kTabTop);
    for (int i = 0; i < 2; ++i) set_color(tab_labels_[i], i == index ? 0x000000 : kColorIdle);

    if (rx_panel_) (tab == Tab::Rx ? lv_obj_clear_flag : lv_obj_add_flag)(rx_panel_, LV_OBJ_FLAG_HIDDEN);
    if (tx_panel_) (tab == Tab::Tx ? lv_obj_clear_flag : lv_obj_add_flag)(tx_panel_, LV_OBJ_FLAG_HIDDEN);

    if (tab == Tab::Tx) {
        build_send_entries();
        send_selection_ = 0;
        refresh_tx_list();
    } else {
        shown_finder_sequence_ = 0;
    }
    update_hint();
}

void UISpectrumPage::select_band(std::size_t index)
{
    if (index >= spectrum::kBands.size()) return;
    band_ = index;
    scanner_.set_band(band_);
    set_config_band(static_cast<int>(band_));
    shown_finder_sequence_ = 0;
}

void UISpectrumPage::build_send_entries()
{
    send_entries_.clear();
    const std::string directory = spectrum_dir();
    DIR *dir = opendir(directory.c_str());
    if (!dir) return;
    std::vector<std::string> names;
    while (dirent *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".sub") == 0) names.push_back(name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) { return a > b; });
    constexpr std::size_t kMaxEntries = 32;
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

void UISpectrumPage::refresh_tx_list()
{
    if (!tx_list_) return;
    if (send_entries_.empty()) {
        lv_label_set_text(tx_list_, "no captures yet\n\ncapture one in RX,\nit shows up here");
        if (tx_detail_) set_text(tx_detail_, "");
        return;
    }
    send_selection_ = std::clamp(send_selection_, 0, static_cast<int>(send_entries_.size()) - 1);
    constexpr int kRows = 10;
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
    lv_label_set_text(tx_list_, text.c_str());

    const SendEntry &sel = send_entries_[send_selection_];
    if (tx_detail_) {
        lv_label_set_text(tx_detail_, trim_mhz(sel.freq_hz).c_str());
        set_color(tx_detail_, sel.allowed ? kColorAccent : kColorRed);
    }
}

void UISpectrumPage::delete_selected()
{
    if (send_entries_.empty()) return;
    send_selection_ = std::clamp(send_selection_, 0, static_cast<int>(send_entries_.size()) - 1);
    const std::string sub = send_entries_[send_selection_].path;
    std::remove(sub.c_str());
    // Remove the paired rtl_433 export too (same basename, .ook).
    if (sub.size() > 4 && sub.compare(sub.size() - 4, 4, ".sub") == 0)
        std::remove((sub.substr(0, sub.size() - 4) + ".ook").c_str());
    show_message("deleted");
    build_send_entries();
    if (send_selection_ >= static_cast<int>(send_entries_.size()))
        send_selection_ = std::max(0, static_cast<int>(send_entries_.size()) - 1);
    refresh_tx_list();
}

void UISpectrumPage::render_rx()
{
    const FinderStatus status = scanner_.finder_status();
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    if (status.sequence != shown_finder_sequence_) {
        shown_finder_sequence_ = status.sequence;
        for (std::size_t i = 0; i < spectrum::kFinderChannels.size(); ++i) {
            const bool used = static_cast<int>(i) < status.count;
            auto vis = [used](lv_obj_t *o) {
                if (o) (used ? lv_obj_remove_flag : lv_obj_add_flag)(o, LV_OBJ_FLAG_HIDDEN);
            };
            vis(finder_freq_[i]);
            vis(finder_value_[i]);
            if (lv_obj_t *bar = finder_bar_[i]) vis(lv_obj_get_parent(bar));
            if (!used) continue;
            const int rssi = status.rssi[i];
            const bool above = rssi >= status.threshold_dbm;
            set_text(finder_freq_[i], trim_mhz(status.hz[i]).c_str());
            if (lv_obj_t *bar = finder_bar_[i]) {
                const int span = spectrum::FinderDetector::kMaxThresholdDbm -
                                 spectrum::FinderDetector::kMinThresholdDbm;
                const int over = std::clamp(rssi - spectrum::FinderDetector::kMinThresholdDbm, 0, span);
                const int width = over * kFinderBarW / span;
                if (lv_obj_get_width(bar) != width) lv_obj_set_width(bar, width);
                const lv_color_t color = lv_color_hex(strength_color(rssi, status.threshold_dbm));
                if (!lv_color_eq(lv_obj_get_style_bg_color(bar, LV_PART_MAIN), color))
                    lv_obj_set_style_bg_color(bar, color, LV_PART_MAIN);
            }
            set_text(finder_value_[i], std::to_string(rssi).c_str());
            set_color(finder_freq_[i], above ? kColorText : kColorIdle);
        }
    }

    if (status.hits.empty()) {
        set_text(hit_freq_, "--");
        set_text(hit_detail_, "press a remote");
        set_text(hit_history_, "");
    } else {
        const FinderStatus::Hit &hit = status.hits.front();
        last_hit_hz_ = hit.hz;
        set_text(hit_freq_, trim_mhz(hit.hz).c_str());
        set_text(hit_detail_, (std::to_string(hit.rssi_dbm) + " dBm " + age_text(now - hit.at_ms)).c_str());
        std::string history;
        for (std::size_t i = 1; i < status.hits.size(); ++i) {
            const FinderStatus::Hit &older = status.hits[i];
            history += trim_mhz(older.hz) + " " + std::to_string(older.rssi_dbm) + "\n";
        }
        set_text(hit_history_, history.c_str());
    }

    if (rx_status_) {
        std::string text;
        uint32_t color = kColorDim;
        if (capture_.active && capture_.automatic) {
            text = "rec " + std::to_string(capture_.packets) + "p";
            color = kColorRed;
        } else if (lv_tick_get() < message_until_ || !scanner_ok_) {
            text = message_;
            color = scanner_ok_ ? kColorText : kColorRed;
        } else {
            text = (scanner_.auto_capture() ? "auto t" : "man t") + std::to_string(status.threshold_dbm);
            color = scanner_.auto_capture() ? kColorAccent : kColorDim;
        }
        set_text(rx_status_, text.c_str());
        set_color(rx_status_, color);
    }
}

void UISpectrumPage::update_hint()
{
    if (!hint_label_) return;
    const char *text = tab_ == Tab::Rx ? "1/2/3 ant  up/dn trig  a auto  b filt  ent cap"
                                       : "up/dn pick  ent send  del rm";
    set_text(hint_label_, text);
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
    capture_ = scanner_.capture_status();
    if (tab_ == Tab::Rx) render_rx();

    // A transient message takes over the hint line on any tab so sending,
    // capturing and errors are visible everywhere, not only in RX.
    if (hint_label_) {
        if (lv_tick_get() < message_until_ && !message_.empty()) {
            set_text(hint_label_, message_.c_str());
            set_color(hint_label_, scanner_ok_ ? kColorAccent : kColorRed);
        } else {
            update_hint();
            set_color(hint_label_, kColorDim);
        }
    }
}

void UISpectrumPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UISpectrumPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item) return;
    const uint32_t key = item->semantic_key ? item->semantic_key : item->key_code;
    const bool repeat = item->key_state == KBD_KEY_REPEATED;
    if (repeat && key != KEY_UP && key != KEY_DOWN) return;
    try {
        page->handle_key(key);
    } catch (...) {
    }
}

void UISpectrumPage::handle_key(uint32_t key)
{
    if (key == KEY_ESC) {
        if (navigate_home) navigate_home();
        return;
    }
    if (key == KEY_TAB) {
        select_tab(tab_ == Tab::Rx ? Tab::Tx : Tab::Rx);
        return;
    }

    switch (tab_) {
    case Tab::Rx:
        switch (key) {
        case KEY_1: select_band(0); return;
        case KEY_2: select_band(1); return;
        case KEY_3: select_band(2); return;
        case KEY_UP:
        case KEY_DOWN:
            scanner_.adjust_finder_threshold(key == KEY_UP ? 3 : -3);
            show_message("trigger " + std::to_string(scanner_.finder_threshold_dbm()) + " dbm");
            return;
        case KEY_A:
            scanner_.set_auto_capture(!scanner_.auto_capture());
            show_message(scanner_.auto_capture() ? "auto capture on" : "auto capture off");
            return;
        case KEY_B:
            scanner_.cycle_capture_bandwidth();
            show_message("filter " + std::to_string(scanner_.capture_bandwidth_khz()) + " khz");
            return;
        case KEY_ENTER:
        case KEY_KPENTER:
            if (scanner_ok_) scanner_.toggle_capture(last_hit_hz_);
            return;
        default:
            return;
        }
    case Tab::Tx:
        switch (key) {
        case KEY_UP:
            if (send_selection_ > 0) {
                --send_selection_;
                refresh_tx_list();
            }
            return;
        case KEY_DOWN:
            if (send_selection_ + 1 < static_cast<int>(send_entries_.size())) {
                ++send_selection_;
                refresh_tx_list();
            }
            return;
        case KEY_ENTER:
        case KEY_KPENTER:
            if (!send_entries_.empty() && !scanner_.transmitting()) {
                const SendEntry &entry = send_entries_[send_selection_];
                if (entry.allowed) {
                    scanner_.request_transmit(entry.path);
                    show_message("sending " + trim_mhz(entry.freq_hz));
                } else {
                    show_message("cannot tx this capture");
                }
            }
            return;
        case KEY_BACKSPACE:
        case KEY_DELETE:
            delete_selected();
            return;
        default:
            return;
        }
    }
}

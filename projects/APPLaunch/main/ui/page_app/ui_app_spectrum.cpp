/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_spectrum.hpp"

#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "../keyboard_text_input.hpp"
#include "keyboard_input.h"
#include "input_keys.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace {

static const uint32_t &kColorAccent = cp0::theme::kAccent;
static const uint32_t &kColorText = cp0::theme::kText;
static const uint32_t &kColorIdle = cp0::theme::kIdle;
static const uint32_t &kColorDim = cp0::theme::kDim;
static const uint32_t &kColorGrid = cp0::theme::kLine;
static const uint32_t &kColorRed = cp0::theme::kDanger;
static const uint32_t &kColorAmber = cp0::theme::kWarn;
constexpr const char *kBandConfigKey = "spectrum_band";
constexpr uint32_t kRefreshMs = 50;
constexpr uint32_t kMessageMs = 2500;

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

    cp0::ui::paint_background(ui_APP_Container);
    create_tabbar();
    create_rx();
    create_tx();
    hint_label_ = cp0::ui::footer(ui_APP_Container);

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
    if (input_context_saved_) {
        cp0_keyboard_set_input_context(static_cast<cp0_keyboard_input_context_t>(previous_input_context_));
        cp0_keyboard_set_lvgl_keypad_intercept(previous_keypad_intercept_);
    }
    scanner_.stop();
}

lv_obj_t *UISpectrumPage::mk_label(lv_obj_t *parent, int x, int y, int px, uint32_t color)
{
    return cp0::ui::label(parent ? parent : ui_APP_Container, x, y, color, px > 11);
}

void UISpectrumPage::create_tabbar()
{
    if (!ui_APP_Container) return;
    const cp0::ui::TitleRow title = cp0::ui::title_row(ui_APP_Container, "spectrum");
    status_ = title.status;
    static const char *const kNames[] = {"rx", "tx"};
    int x = kTabX;
    for (int i = 0; i < 2; ++i) {
        tab_labels_[i] = mk_label(ui_APP_Container, x, cp0::theme::kTitleY, 11, kColorIdle);
        if (tab_labels_[i]) lv_label_set_text(tab_labels_[i], kNames[i]);
        tab_lines_[i] = cp0::ui::box(ui_APP_Container, x - 1, 13, 16, 1, kColorAccent);
        x += 24;
    }
}

void UISpectrumPage::create_rx()
{
    rx_panel_ = cp0::ui::box(ui_APP_Container, 0, kBodyTop, kWidth, kBodyH, cp0::theme::kBg);
    if (!rx_panel_) return;
    lv_obj_set_style_bg_opa(rx_panel_, LV_OPA_TRANSP, 0);
    cp0::ui::dot_grid(rx_panel_, 0, 44, kWidth, kBodyH - 44);

    hit_ = cp0::ui::readout(rx_panel_, 4, 2, 162, 40, "hit  MHz");
    peak_ = cp0::ui::readout(rx_panel_, 170, 2, 146, 40, "peak  dBm");
    if (peak_.panel.box) {
        mode_ = cp0::ui::label(peak_.panel.box, 146 - 5 - 60, 2, kColorAccent);
        if (mode_) {
            lv_obj_set_width(mode_, 60);
            lv_obj_set_style_text_align(mode_, LV_TEXT_ALIGN_RIGHT, 0);
        }
    }

    for (std::size_t i = 0; i < spectrum::kFinderChannels.size(); ++i) {
        const int y = 46 + static_cast<int>(i) * 17;
        finder_freq_[i] = mk_label(rx_panel_, 4, y + 2, 11, kColorIdle);
        finder_bar_[i] = cp0::ui::segbar(rx_panel_, kBarX, y + 4, kBarW, 7, cp0::theme::kBg);
        finder_value_[i] = mk_label(rx_panel_, kWidth - 4 - 36, y + 2, 11, kColorIdle);
        if (finder_value_[i]) {
            lv_obj_set_width(finder_value_[i], 36);
            lv_obj_set_style_text_align(finder_value_[i], LV_TEXT_ALIGN_RIGHT, 0);
        }
    }
    trigger_line_ = cp0::ui::tiled(rx_panel_, kBarX, 44, 1, 88, cp0::ui::pattern(1, 2, kColorText));
    trigger_label_ = mk_label(rx_panel_, kBarX, 122, 11, cp0::theme::kDim);
}

void UISpectrumPage::create_tx()
{
    tx_panel_ = cp0::ui::box(ui_APP_Container, 0, kBodyTop, kWidth, kBodyH, cp0::theme::kBg);
    if (!tx_panel_) return;
    lv_obj_set_style_bg_opa(tx_panel_, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(tx_panel_, LV_OBJ_FLAG_HIDDEN);

    tx_freq_ = cp0::ui::readout(tx_panel_, 4, 2, 162, 40, "freq  MHz");
    const cp0::ui::Panel info = cp0::ui::panel(tx_panel_, 170, 2, 146, 40);
    if (info.box) {
        if (lv_obj_t *caption = cp0::ui::label(info.box, 5, 2, cp0::theme::kDim)) lv_label_set_text(caption, "signal");
        tx_signal_ = cp0::ui::label(info.box, 5, 15, kColorText);
        tx_length_ = cp0::ui::label(info.box, 5, 26, kColorIdle);
    }
    const cp0::ui::Panel list = cp0::ui::panel(tx_panel_, 4, 46, 312, 88);
    if (!list.box) return;
    tx_empty_ = cp0::ui::label(list.box, 8, 8, kColorIdle);
    if (tx_empty_) lv_label_set_text(tx_empty_, "no captures yet. capture one in rx\nand it shows up here.");
    for (int i = 0; i < kTxRows; ++i) {
        TxRow &row = tx_rows_[i];
        row.row = cp0::ui::box(list.box, 1, 3 + i * 21, 310, 19, cp0::theme::kSelect);
        if (!row.row) continue;
        row.mark = cp0::ui::box(row.row, 0, 0, 2, 19, kColorAccent);
        row.time = cp0::ui::label(row.row, 8, 4, kColorText);
        row.freq = cp0::ui::label(row.row, 108, 4, kColorText);
        row.pulses = cp0::ui::label(row.row, 310 - 8 - 90, 4, kColorIdle);
        if (row.pulses) {
            lv_obj_set_width(row.pulses, 90);
            lv_obj_set_style_text_align(row.pulses, LV_TEXT_ALIGN_RIGHT, 0);
        }
    }

    rename_panel_ = cp0::ui::panel(tx_panel_, 4, 46, 312, 88);
    if (rename_panel_.box) {
        cp0::ui::set_panel_focus(rename_panel_, true);
        if (lv_obj_t *caption = cp0::ui::label(rename_panel_.box, 8, 6, cp0::theme::kAccent))
            lv_label_set_text(caption, "rename capture");
        rename_value_ = cp0::ui::label(rename_panel_.box, 8, 28, kColorText, true);
        rename_cursor_ = cp0::ui::box(rename_panel_.box, 8, 28 + 6, 9, 16, kColorAccent);
        if (lv_obj_t *rule = cp0::ui::label(rename_panel_.box, 8, 62, kColorIdle))
            lv_label_set_text(rule, "a-z 0-9 - _  up to 13");
        lv_obj_add_flag(rename_panel_.box, LV_OBJ_FLAG_HIDDEN);
    }
}

void UISpectrumPage::start_rename()
{
    if (send_entries_.empty() || !rename_panel_.box) return;
    const std::string &label = send_entries_[send_selection_].label;
    rename_text_ = label.size() >= 15 && label[8] == '-' ? std::string() : label;
    renaming_ = true;
    if (!input_context_saved_) {
        previous_input_context_ = cp0_keyboard_get_input_context();
        previous_keypad_intercept_ = cp0_keyboard_get_lvgl_keypad_intercept();
        input_context_saved_ = true;
    }
    cp0_keyboard_set_input_context(KBD_INPUT_CONTEXT_TEXT);
    cp0_keyboard_set_lvgl_keypad_intercept(1);
    lv_obj_clear_flag(rename_panel_.box, LV_OBJ_FLAG_HIDDEN);
    render_rename();
    cp0::ui::set_footer(hint_label_, {{"ent", "save"}, {"bs", "del"}, {"esc", "cancel"}});
}

void UISpectrumPage::render_rename()
{
    set_text(rename_value_, rename_text_.c_str());
    if (rename_cursor_ && rename_value_) {
        lv_obj_update_layout(rename_value_);
        lv_obj_set_x(rename_cursor_, 9 + (rename_text_.empty() ? 0 : lv_obj_get_width(rename_value_)));
    }
}

void UISpectrumPage::finish_rename(bool save)
{
    renaming_ = false;
    if (input_context_saved_) {
        cp0_keyboard_set_input_context(static_cast<cp0_keyboard_input_context_t>(previous_input_context_));
        cp0_keyboard_set_lvgl_keypad_intercept(previous_keypad_intercept_);
        input_context_saved_ = false;
    }
    if (rename_panel_.box) lv_obj_add_flag(rename_panel_.box, LV_OBJ_FLAG_HIDDEN);
    update_hint();
    if (!save || send_entries_.empty()) return;

    const std::string name = spectrum::capture_name(rename_text_);
    if (name.empty()) {
        show_message("name needs a letter or digit");
        return;
    }
    const std::string old_sub = send_entries_[send_selection_].path;
    const std::string new_sub = spectrum_dir() + "/" + name + ".sub";
    if (new_sub == old_sub) return;
    struct stat info{};
    if (stat(new_sub.c_str(), &info) == 0) {
        show_message("name taken: " + name);
        return;
    }
    if (std::rename(old_sub.c_str(), new_sub.c_str()) != 0) {
        show_message("rename failed");
        return;
    }
    const std::string old_ook = old_sub.substr(0, old_sub.size() - 4) + ".ook";
    if (stat(old_ook.c_str(), &info) == 0) std::rename(old_ook.c_str(), (new_sub.substr(0, new_sub.size() - 4) + ".ook").c_str());
    build_send_entries();
    for (std::size_t i = 0; i < send_entries_.size(); ++i)
        if (send_entries_[i].path == new_sub) send_selection_ = static_cast<int>(i);
    refresh_tx_list();
    show_message("renamed " + name);
}

void UISpectrumPage::handle_rename_key(uint32_t key, const struct key_item *item)
{
    switch (key) {
    case KEY_ENTER:
    case KEY_KPENTER:
        finish_rename(true);
        return;
    case KEY_ESC:
        finish_rename(false);
        return;
    case KEY_BACKSPACE:
        if (!rename_text_.empty()) rename_text_.pop_back();
        break;
    default: {
        const std::string typed = spectrum::capture_name(rename_text_ + launcher_ui::text_input::key_text(key, item));
        if (typed.size() > rename_text_.size()) rename_text_ = typed;
        break;
    }
    }
    render_rename();
}

void UISpectrumPage::select_tab(Tab tab)
{
    tab_ = tab;
    const int index = tab == Tab::Rx ? 0 : 1;
    for (int i = 0; i < 2; ++i) {
        set_color(tab_labels_[i], i == index ? kColorAccent : kColorIdle);
        if (tab_lines_[i]) (i == index ? lv_obj_clear_flag : lv_obj_add_flag)(tab_lines_[i], LV_OBJ_FLAG_HIDDEN);
    }

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
    std::vector<std::pair<long, std::string>> dated;
    for (const std::string &name : names) {
        struct stat info{};
        const long mtime = stat((directory + "/" + name).c_str(), &info) == 0 ? static_cast<long>(info.st_mtime) : 0;
        dated.emplace_back(mtime, name);
    }
    std::sort(dated.begin(), dated.end(), [](const auto &a, const auto &b) {
        return a.first != b.first ? a.first > b.first : a.second > b.second;
    });
    constexpr std::size_t kMaxEntries = 32;
    if (dated.size() > kMaxEntries) dated.resize(kMaxEntries);
    for (const auto &[mtime, name] : dated) {
        const std::string path = directory + "/" + name;
        const spectrum::Capture capture = spectrum::parse_sub(read_file(path));
        SendEntry entry;
        entry.path = path;
        entry.freq_hz = capture.freq_hz;
        entry.allowed = spectrum::tx_allowed(capture.freq_hz) && !capture.durations.empty();
        uint64_t total_us = 0;
        for (int32_t d : capture.durations) {
            if (d > 0) ++entry.pulses;
            total_us += static_cast<uint64_t>(d < 0 ? -static_cast<int64_t>(d) : d);
        }
        entry.duration_ms = static_cast<uint32_t>(total_us / 1000);
        entry.mtime = mtime;
        std::string label = name;
        if (label.rfind("capture-", 0) == 0) label = label.substr(8);
        if (label.size() > 4) label.resize(label.size() - 4);
        entry.label = std::move(label);
        send_entries_.push_back(std::move(entry));
    }
}

// "20261115-215411-433.920" -> "21:54:11"; anything else is shown as-is.
static std::string capture_time(const std::string &label)
{
    if (label.size() >= 15 && label[8] == '-')
        return label.substr(9, 2) + ":" + label.substr(11, 2) + ":" + label.substr(13, 2);
    return label;
}

void UISpectrumPage::refresh_tx_list()
{
    if (status_ && tab_ == Tab::Tx)
        set_text(status_, (std::to_string(send_entries_.size()) + " saved").c_str());
    const bool empty = send_entries_.empty();
    if (tx_empty_) (empty ? lv_obj_clear_flag : lv_obj_add_flag)(tx_empty_, LV_OBJ_FLAG_HIDDEN);
    if (empty) {
        for (TxRow &row : tx_rows_)
            if (row.row) lv_obj_add_flag(row.row, LV_OBJ_FLAG_HIDDEN);
        cp0::ui::set_readout(tx_freq_, "--", "", kColorIdle);
        if (tx_signal_) set_text(tx_signal_, "");
        if (tx_length_) set_text(tx_length_, "");
        return;
    }
    send_selection_ = std::clamp(send_selection_, 0, static_cast<int>(send_entries_.size()) - 1);
    int first = std::max(0, send_selection_ - kTxRows / 2);
    if (first + kTxRows > static_cast<int>(send_entries_.size()))
        first = std::max(0, static_cast<int>(send_entries_.size()) - kTxRows);
    for (int i = 0; i < kTxRows; ++i) {
        TxRow &row = tx_rows_[i];
        if (!row.row) continue;
        const int index = first + i;
        if (index >= static_cast<int>(send_entries_.size())) {
            lv_obj_add_flag(row.row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(row.row, LV_OBJ_FLAG_HIDDEN);
        const SendEntry &entry = send_entries_[index];
        const bool on = index == send_selection_;
        lv_obj_set_style_bg_opa(row.row, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (row.mark) (on ? lv_obj_clear_flag : lv_obj_add_flag)(row.mark, LV_OBJ_FLAG_HIDDEN);
        set_text(row.time, capture_time(entry.label).c_str());
        set_color(row.time, on ? kColorText : kColorIdle);
        set_text(row.freq, trim_mhz(entry.freq_hz).c_str());
        set_color(row.freq, entry.allowed ? (on ? kColorText : kColorIdle) : kColorRed);
        set_text(row.pulses, (std::to_string(entry.pulses) + " pk").c_str());
    }

    const SendEntry &sel = send_entries_[send_selection_];
    cp0::ui::set_readout(tx_freq_, trim_mhz(sel.freq_hz), "", sel.allowed ? kColorText : kColorRed);
    if (tx_signal_) set_text(tx_signal_, ("ook  " + std::to_string(sel.pulses) + " pulses").c_str());
    if (tx_length_) {
        char text[24];
        std::snprintf(text, sizeof(text), "%.1f s%s", sel.duration_ms / 1000.0, sel.allowed ? "" : "  no tx");
        set_text(tx_length_, text);
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
    const int span = spectrum::FinderDetector::kMaxThresholdDbm - spectrum::FinderDetector::kMinThresholdDbm;
    const auto bar_fraction = [span](int dbm) {
        return static_cast<float>(std::clamp(dbm - spectrum::FinderDetector::kMinThresholdDbm, 0, span)) /
               static_cast<float>(span);
    };
    if (status.sequence != shown_finder_sequence_) {
        shown_finder_sequence_ = status.sequence;
        for (std::size_t i = 0; i < spectrum::kFinderChannels.size(); ++i) {
            const bool used = static_cast<int>(i) < status.count;
            auto vis = [used](lv_obj_t *o) {
                if (o) (used ? lv_obj_remove_flag : lv_obj_add_flag)(o, LV_OBJ_FLAG_HIDDEN);
            };
            vis(finder_freq_[i]);
            vis(finder_value_[i]);
            vis(finder_bar_[i].track);
            if (!used) continue;
            const int rssi = status.rssi[i];
            const bool above = rssi >= status.threshold_dbm;
            set_text(finder_freq_[i], trim_mhz(status.hz[i]).c_str());
            cp0::ui::set_segbar(finder_bar_[i], bar_fraction(rssi), strength_color(rssi, status.threshold_dbm));
            set_text(finder_value_[i], std::to_string(rssi).c_str());
            set_color(finder_freq_[i], above ? kColorText : kColorIdle);
            set_color(finder_value_[i], above ? kColorText : kColorIdle);
        }
        const int line_x = kBarX + static_cast<int>(bar_fraction(status.threshold_dbm) * kBarW);
        if (trigger_line_ && lv_obj_get_x(trigger_line_) != line_x) lv_obj_set_x(trigger_line_, line_x);
        if (trigger_label_) {
            set_text(trigger_label_, ("trig " + std::to_string(status.threshold_dbm)).c_str());
            lv_obj_set_x(trigger_label_, std::min(line_x + 3, kWidth - 4 - 56));
        }
    }

    if (status.hits.empty()) {
        cp0::ui::set_readout(hit_, "--", "", kColorIdle);
        cp0::ui::set_readout(peak_, "--", "", kColorIdle);
        if (hit_.label) set_text(hit_.label, "hit  MHz");
    } else {
        const FinderStatus::Hit &hit = status.hits.front();
        last_hit_hz_ = hit.hz;
        cp0::ui::set_readout(hit_, trim_mhz(hit.hz), "", kColorText);
        cp0::ui::set_readout(peak_, std::to_string(hit.rssi_dbm), "", kColorAccent);
        if (hit_.label) set_text(hit_.label, ("hit  MHz  " + age_text(now - hit.at_ms)).c_str());
    }
    if (mode_) {
        set_text(mode_, scanner_.auto_capture() ? "auto" : "manual");
        set_color(mode_, scanner_.auto_capture() ? kColorAccent : kColorIdle);
    }

    if (status_) {
        if (capture_.active && capture_.automatic) {
            set_text(status_, ("rec " + std::to_string(capture_.packets) + " pk").c_str());
            set_color(status_, kColorRed);
        } else if (!scanner_ok_) {
            set_text(status_, "no radio");
            set_color(status_, kColorRed);
        } else {
            set_text(status_, spectrum::kBands[band_].label);
            set_color(status_, kColorIdle);
        }
    }
}

void UISpectrumPage::update_hint()
{
    if (renaming_) {
        cp0::ui::set_footer(hint_label_, {{"ent", "save"}, {"bs", "del"}, {"esc", "cancel"}});
        return;
    }
    if (tab_ == Tab::Rx)
        cp0::ui::set_footer(hint_label_, {{"ent", "cap"}, {"a", "auto"}, {"up/dn", "trig"}, {"1-3", "ant"}, {"b", "filt"}});
    else
        cp0::ui::set_footer(hint_label_, {{"ent", "send"}, {"up/dn", "pick"}, {"n", "rename"}, {"del", "rm"}});
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
        if (lv_tick_get() < message_until_ && !message_.empty())
            cp0::ui::set_footer_message(hint_label_, message_.c_str(), scanner_ok_ ? kColorAccent : kColorRed);
        else
            update_hint();
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
    try {
        if (page->renaming_) {
            if (!repeat || key == KEY_BACKSPACE) page->handle_rename_key(key, item);
            return;
        }
        if (repeat && key != KEY_UP && key != KEY_DOWN) return;
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
        case KEY_N:
            start_rename();
            return;
        default:
            return;
        }
    }
}

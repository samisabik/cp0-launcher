/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_sensors.hpp"

#include "../app_support.hpp"
#include "../model/sdr_model.hpp"
#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "input_keys.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr uint32_t kRefreshMs = 200;
constexpr uint32_t kRenderEveryMs = 1000;
constexpr uint32_t kMessageMs = 3000;
constexpr uint32_t kDetectEveryMs = 3000;
constexpr uint32_t kDetectTimeoutMs = 10000;
constexpr long kOutputLimit = 1 << 20;
constexpr int kNameChars = 19;
constexpr const char *kDetectDone = "cp0-detect-done";
constexpr const char *kFrequencyConfigKey = "sensors_freq";

double now_seconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int config_frequency()
{
    int value = 0;
    cp0_signal_config_api({"GetInt", kFrequencyConfigKey, "0"}, [&](int code, std::string data) {
        if (code == 0) value = std::atoi(data.c_str());
    });
    return std::clamp(value, 0, static_cast<int>(sensors::kFrequenciesHz.size()) - 1);
}

void set_config_frequency(int index)
{
    cp0_signal_config_api({"SetInt", kFrequencyConfigKey, std::to_string(index)}, nullptr);
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

UISensorsPage::UISensorsPage() : AppPage()
{
    set_page_title("Sensors");
    reclaim_top_bar();
    frequency_index_ = config_frequency();
    detect_path_ = app_support::runtime_dir() + "/cp0-sensors-detect";
    out_path_ = app_support::runtime_dir() + "/cp0-sensors-out";
    for (RowView &row : rows_)
        row.pixels.assign(static_cast<std::size_t>(kSparkW) * kSparkH, app_support::rgb565(cp0::theme::kPanel));
    tools_ok_ = app_support::tool_present("rtl_433");
    can_detect_ = app_support::tool_present("rtl_test");
    create_ui();
    render();
    if (tools_ok_) {
        if (can_detect_) {
            start_detect();
        } else {
            // Without rtl_test the only probe is rtl_433 itself: it is
            // started and taken as "no dongle" when it exits.
            present_ = true;
            start_receiver();
        }
    }
    timer_ = lv_timer_create(timer_cb, kRefreshMs, this);
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

UISensorsPage::~UISensorsPage()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
    stop_receiver();
    app_support::stop(detect_pid_);
    unlink(detect_path_.c_str());
    unlink(out_path_.c_str());
}

void UISensorsPage::create_ui()
{
    if (!ui_APP_Container) return;
    cp0::ui::paint_background(ui_APP_Container);
    const cp0::ui::TitleRow title = cp0::ui::title_row(ui_APP_Container, "sensors");
    status_ = title.status;

    for (int i = 0; i < kRows; ++i) {
        RowView &row = rows_[i];
        row.panel = cp0::ui::panel(ui_APP_Container, 4, 19 + i * 33, 312, 31);
        if (!row.panel.box) continue;
        row.temperature = cp0::ui::label(row.panel.box, 3, 3, cp0::theme::kText, true);
        if (row.temperature) {
            lv_obj_set_width(row.temperature, 58);
            lv_obj_set_style_text_align(row.temperature, LV_TEXT_ALIGN_RIGHT, 0);
        }
        row.unit = cp0::ui::label(row.panel.box, 64, 16, cp0::theme::kIdle);
        if (row.unit) lv_label_set_text(row.unit, "C");
        row.name = cp0::ui::label(row.panel.box, 80, 4, cp0::theme::kDim);
        if (row.name) {
            lv_obj_set_width(row.name, kNameChars * 7);
            lv_label_set_long_mode(row.name, LV_LABEL_LONG_CLIP);
        }
        row.humidity = cp0::ui::label(row.panel.box, 80, 17, cp0::theme::kIdle);
        row.battery = cp0::ui::label(row.panel.box, 115, 17, cp0::theme::kWarn);
        row.spark = lv_canvas_create(row.panel.box);
        if (row.spark) {
            lv_canvas_set_buffer(row.spark, row.pixels.data(), kSparkW, kSparkH, LV_COLOR_FORMAT_RGB565);
            lv_obj_set_pos(row.spark, 218, 3);
        }
        row.age = cp0::ui::label(row.panel.box, 272, 17, cp0::theme::kDim);
        if (row.age) {
            lv_obj_set_width(row.age, 35);
            lv_obj_set_style_text_align(row.age, LV_TEXT_ALIGN_RIGHT, 0);
        }
        set_hidden(row.panel.box, true);
    }

    big_ = cp0::ui::label(ui_APP_Container, 12, 60, cp0::theme::kText, true);
    sub_ = cp0::ui::label(ui_APP_Container, 12, 92, cp0::theme::kIdle);
    hint_ = cp0::ui::footer(ui_APP_Container);
}

void UISensorsPage::show_message(const std::string &text)
{
    message_ = text;
    message_until_ = lv_tick_get() + kMessageMs;
    render();
}

void UISensorsPage::start_detect()
{
    if (detect_pid_ > 0) return;
    last_detect_ = lv_tick_get();
    unlink(detect_path_.c_str());
    // The marker is written after rtl_test has exited and released the
    // dongle, so rtl_433 never starts while the probe still holds it.
    detect_pid_ = app_support::spawn("( " + sdr::detect_cmd() + "; echo " + kDetectDone + " ) > '" + detect_path_ +
                                     "' 2>&1");
}

void UISensorsPage::poll_detect()
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

void UISensorsPage::start_receiver()
{
    stop_receiver();
    out_offset_ = 0;
    pending_.clear();
    if (std::FILE *file = std::fopen(out_path_.c_str(), "w")) std::fclose(file);
    // Output goes to a regular file, so rtl_433 is never blocked on a pipe
    // when it is signalled to stop and can close the dongle.
    proc_pid_ = app_support::spawn(
        sensors::command(sensors::kFrequenciesHz[static_cast<std::size_t>(frequency_index_)], out_path_));
    if (proc_pid_ <= 0) present_ = false;
}

void UISensorsPage::stop_receiver()
{
    app_support::stop(proc_pid_);
}

bool UISensorsPage::read_lines()
{
    pending_ += tail_file(out_path_, out_offset_);
    const double now = now_seconds();
    std::size_t start = 0;
    bool changed = false;
    bool exited = false;
    while (true) {
        const std::size_t nl = pending_.find('\n', start);
        if (nl == std::string::npos) break;
        const std::string line = pending_.substr(start, nl - start);
        start = nl + 1;
        if (line.compare(0, std::strlen(sensors::kExitMark), sensors::kExitMark) == 0)
            exited = true;
        else if (table_.add_line(line, now))
            changed = true;
    }
    pending_.erase(0, start);
    if (pending_.size() > 4096) pending_.clear();

    if (exited) {
        stop_receiver();
        present_ = false;
        last_detect_ = lv_tick_get();
        return true;
    }
    // rtl_433 appends, so emptying the file under it keeps the scratch file
    // small; a line written between the read above and here is lost.
    if (out_offset_ > kOutputLimit && pending_.empty() && truncate(out_path_.c_str(), 0) == 0) out_offset_ = 0;
    return changed;
}

void UISensorsPage::set_frequency(int index)
{
    const int count = static_cast<int>(sensors::kFrequenciesHz.size());
    index = (index % count + count) % count;
    if (index == frequency_index_) return;
    frequency_index_ = index;
    set_config_frequency(index);
    if (present_) start_receiver();
    show_message(sensors::format_frequency(sensors::kFrequenciesHz[static_cast<std::size_t>(index)]) + " mhz");
}

void UISensorsPage::save_log()
{
    if (table_.sensors().empty()) {
        show_message("nothing to save");
        return;
    }
    const std::string directory = app_support::home_dir() + "/sensors";
    if (mkdir(directory.c_str(), 0755) != 0 && errno != EEXIST) {
        show_message("cannot create ~/sensors");
        return;
    }
    std::FILE *file = std::fopen((directory + "/sensors.log").c_str(), "a");
    if (!file) {
        show_message("cannot write log");
        return;
    }
    char stamp[32] = "";
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (localtime_r(&now, &local)) std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    for (const sensors::Sensor &sensor : table_.sensors())
        std::fprintf(file, "%s\n", sensors::format_log_line(sensor, stamp).c_str());
    std::fclose(file);
    show_message("saved log");
}

void UISensorsPage::draw_sparkline(RowView &row, const sensors::Sensor &sensor, bool focused)
{
    if (!row.spark) return;
    const uint16_t bg = app_support::rgb565(focused ? cp0::theme::kSelect : cp0::theme::kPanel);
    const uint16_t ink = app_support::rgb565(focused ? cp0::theme::kAccent : cp0::theme::kDim);
    std::vector<uint16_t> next(row.pixels.size(), bg);
    const std::vector<int> columns = sensors::sparkline(sensor.history(), kSparkW, kSparkH);
    int previous = -1;
    for (int x = 0; x < kSparkW; ++x) {
        const int y = columns[static_cast<std::size_t>(x)];
        if (y < 0) continue;
        // Join to the previous column so steps do not leave gaps.
        const int from = previous < 0 ? y : std::min(previous, y);
        const int to = previous < 0 ? y : std::max(previous, y);
        for (int fill = from; fill <= to; ++fill) next[static_cast<std::size_t>(fill) * kSparkW + x] = ink;
        previous = y;
    }
    if (next == row.pixels) return;
    // Copy in place: the canvas keeps pointing at this buffer.
    std::copy(next.begin(), next.end(), row.pixels.begin());
    lv_obj_invalidate(row.spark);
}

void UISensorsPage::render()
{
    last_render_ = lv_tick_get();
    const std::vector<sensors::Sensor> &all = table_.sensors();
    const int count = static_cast<int>(all.size());
    const bool receiving = present_ && proc_pid_ > 0;
    const std::string mhz = sensors::format_frequency(sensors::kFrequenciesHz[static_cast<std::size_t>(frequency_index_)]);

    if (status_) {
        std::string text = mhz + "  " + std::to_string(count) + " seen";
        uint32_t color = receiving ? cp0::theme::kAccent : cp0::theme::kIdle;
        if (lv_tick_get() < message_until_) {
            text = message_;
            color = cp0::theme::kText;
        } else if (!tools_ok_) {
            text = "not installed";
            color = cp0::theme::kDanger;
        } else if (!receiving) {
            text = "no dongle";
        }
        cp0::ui::set_text(status_, text.c_str(), color);
    }

    selected_ = std::clamp(selected_, 0, std::max(0, count - 1));
    scroll_ = std::clamp(scroll_, 0, std::max(0, count - kRows));
    if (selected_ < scroll_) scroll_ = selected_;
    if (selected_ >= scroll_ + kRows) scroll_ = selected_ - kRows + 1;

    const double now = now_seconds();
    for (int i = 0; i < kRows; ++i) {
        RowView &row = rows_[i];
        const int index = scroll_ + i;
        const bool used = index < count;
        set_hidden(row.panel.box, !used);
        if (!used) continue;
        const sensors::Sensor &sensor = all[static_cast<std::size_t>(index)];
        const sensors::Reading &reading = sensor.latest;
        const bool focused = index == selected_;
        cp0::ui::set_panel_focus(row.panel, focused);
        cp0::ui::set_text(row.temperature, sensors::format_temperature(reading).c_str(),
                          focused ? cp0::theme::kAccent : cp0::theme::kText);
        cp0::ui::set_text(row.name, sensors::format_name(reading, kNameChars).c_str(), cp0::theme::kDim);
        cp0::ui::set_text(row.humidity, reading.has_humidity ? (std::to_string(reading.humidity) + "%").c_str() : "",
                          cp0::theme::kIdle);
        cp0::ui::set_text(row.battery, reading.battery_ok == 0 ? "low batt" : "", cp0::theme::kWarn);
        cp0::ui::set_text(row.age, sensors::format_age(now - sensor.last_seen).c_str(), cp0::theme::kDim);
        draw_sparkline(row, sensor, focused);
    }

    const bool empty = count == 0;
    set_hidden(big_, !empty);
    set_hidden(sub_, !empty);
    if (empty) {
        if (!tools_ok_) {
            cp0::ui::set_text(big_, "rtl_433 not installed", cp0::theme::kText);
            cp0::ui::set_text(sub_, "run: sudo apt install rtl-433", cp0::theme::kIdle);
        } else if (!receiving) {
            cp0::ui::set_text(big_, "no sdr", cp0::theme::kText);
            cp0::ui::set_text(sub_, "plug in the rtl-sdr dongle", cp0::theme::kIdle);
        } else {
            cp0::ui::set_text(big_, "listening", cp0::theme::kText);
            cp0::ui::set_text(sub_, ("waiting for sensors on " + mhz + " mhz").c_str(), cp0::theme::kIdle);
        }
    }

    cp0::ui::set_footer(hint_, {{"up/dn", "select"}, {"< >", "band"}, {"s", "log"}});
}

void UISensorsPage::timer_cb(lv_timer_t *timer) noexcept
{
    auto *page = static_cast<UISensorsPage *>(lv_timer_get_user_data(timer));
    if (!page) return;
    try {
        page->on_timer();
    } catch (...) {
    }
}

void UISensorsPage::on_timer()
{
    const uint32_t tick = lv_tick_get();
    bool changed = false;
    if (message_until_ != 0 && tick >= message_until_) {
        message_until_ = 0;
        changed = true;
    }
    if (tools_ok_ && !present_ && detect_pid_ <= 0 && tick - last_detect_ >= kDetectEveryMs) {
        if (can_detect_) {
            start_detect();
        } else {
            present_ = true;
            start_receiver();
            last_detect_ = tick;
            changed = true;
        }
    }
    if (detect_pid_ > 0) poll_detect();
    if (present_ && proc_pid_ > 0 && read_lines()) changed = true;
    // Ages tick over even when nothing new arrives.
    if (changed || tick - last_render_ >= kRenderEveryMs) render();
}

void UISensorsPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UISensorsPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item) return;
    const uint32_t key = item->semantic_key ? item->semantic_key : item->key_code;
    try {
        page->handle_key(key, item->key_state == KBD_KEY_REPEATED);
    } catch (...) {
    }
}

void UISensorsPage::handle_key(uint32_t key, bool repeat)
{
    if (key == KEY_ESC) {
        if (navigate_home) navigate_home();
        return;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        selected_ += key == KEY_DOWN ? 1 : -1;
        render();
        return;
    }
    if (repeat) return;
    if (key == KEY_LEFT || key == KEY_RIGHT) {
        set_frequency(frequency_index_ + (key == KEY_RIGHT ? 1 : -1));
    } else if (key == KEY_S) {
        save_log();
    }
}

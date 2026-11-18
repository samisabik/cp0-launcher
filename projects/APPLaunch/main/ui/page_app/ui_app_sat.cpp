/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_sat.hpp"

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
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr uint32_t kRefreshMs = 250;
constexpr uint32_t kMessageMs = 3000;
constexpr uint32_t kPopupMs = 5000;
constexpr uint32_t kStatEveryMs = 2000;
constexpr uint32_t kLocationEveryMs = 5000;
constexpr uint32_t kUpdateTimeoutMs = 120000;
constexpr uint32_t kDetectTimeoutMs = 10000;
constexpr double kRecomputeEveryS = 900.0;
constexpr double kSearchHours = 24.0;
constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kMaxTracked = 24;
constexpr std::size_t kMaxPasses = 40;
constexpr int kArcPoints = 64;
constexpr int kNameChars = 11;
constexpr const char *kDetectDone = "cp0-detect-done";

double unix_now()
{
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

std::string local_time(double unix_time, const char *format)
{
    const std::time_t when = static_cast<std::time_t>(unix_time);
    std::tm local{};
    char text[32] = "";
    if (localtime_r(&when, &local)) std::strftime(text, sizeof(text), format, &local);
    return text;
}

void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == hidden) return;
    if (hidden)
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

sat::Observer observer_of(const location::Position &position)
{
    sat::Observer observer;
    observer.lat_deg = position.lat;
    observer.lon_deg = position.lon;
    return observer;
}

} // namespace

UISatPage::UISatPage() : AppPage()
{
    set_page_title("Sat");
    reclaim_top_bar();
    const std::string directory = app_support::home_dir() + "/sat";
    tle_path_ = directory + "/tle.txt";
    watch_path_ = directory + "/watch.txt";
    const std::string runtime = app_support::runtime_dir();
    update_status_path_ = runtime + "/cp0-sat-update";
    detect_path_ = runtime + "/cp0-sat-detect";
    record_status_path_ = runtime + "/cp0-sat-record";
    pixels_.assign(static_cast<std::size_t>(kCanvasW) * kCanvasH, app_support::rgb565(cp0::theme::kPanel));
    load_location();
    stat_tle();
    create_ui();
    show_popup(true);
    const double now = unix_now();
    render(now);
    draw_sky(now);
    want_compute_ = true;
    timer_ = lv_timer_create(timer_cb, kRefreshMs, this);
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

UISatPage::~UISatPage()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
    worker_stop_ = true;
    if (worker_.joinable()) worker_.join();
    if (record_pid_ > 0) finish_record({});
    app_support::stop(detect_pid_);
    app_support::stop(update_pid_);
    unlink(detect_path_.c_str());
    unlink(update_status_path_.c_str());
    unlink(record_status_path_.c_str());
}

void UISatPage::create_ui()
{
    if (!ui_APP_Container) return;
    cp0::ui::paint_background(ui_APP_Container);
    const cp0::ui::TitleRow row = cp0::ui::title_row(ui_APP_Container, "sat");
    title_ = row.title;
    status_ = row.status;

    cp0::ui::panel(ui_APP_Container, 4, 19, 134, 132);
    canvas_ = lv_canvas_create(ui_APP_Container);
    if (canvas_) {
        lv_canvas_set_buffer(canvas_, pixels_.data(), kCanvasW, kCanvasH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(canvas_, kCanvasX, kCanvasY);
    }
    static constexpr struct { int x, y; const char *text; } kCompass[] = {
        {68, 20, "n"}, {68, 138, "s"}, {8, 80, "w"}, {128, 80, "e"}};
    for (const auto &point : kCompass) {
        lv_obj_t *label = cp0::ui::label(ui_APP_Container, point.x, point.y, cp0::theme::kDim);
        if (label) lv_label_set_text(label, point.text);
    }

    next_ = cp0::ui::readout(ui_APP_Container, 142, 19, 174, 40, "next pass");
    const cp0::ui::Panel list = cp0::ui::panel(ui_APP_Container, 142, 63, 174, 88);
    if (list.box) {
        lv_obj_t *header = cp0::ui::label(list.box, 3, 3, cp0::theme::kDim);
        if (header) lv_label_set_text(header, "sat         aos   el dur");
        cp0::ui::dotted_rule(list.box, 4, 15, 166);
        select_bar_ = cp0::ui::box(list.box, 1, 18, 172, 12, cp0::theme::kSelect);
        set_hidden(select_bar_, true);
        for (int i = 0; i < kRows; ++i) {
            row_labels_[i] = cp0::ui::label(list.box, 3, 19 + i * 13, cp0::theme::kText);
            if (row_labels_[i]) {
                lv_obj_set_width(row_labels_[i], 168);
                lv_label_set_long_mode(row_labels_[i], LV_LABEL_LONG_CLIP);
            }
        }
        empty_ = cp0::ui::label(list.box, 5, 20, cp0::theme::kIdle);
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
        for (int i = 0; i < 3; ++i)
            popup_lines_[i] = cp0::ui::label(popup_.box, 8, 5 + i * 13, i == 0 ? cp0::theme::kAccent : cp0::theme::kText);
        set_hidden(popup_.box, true);
    }
}

void UISatPage::show_message(const std::string &text)
{
    message_ = text;
    message_until_ = lv_tick_get() + kMessageMs;
}

void UISatPage::show_popup(bool on)
{
    popup_until_ = on ? lv_tick_get() + kPopupMs : 0;
    if (on) {
        // The hint follows the selected satellite's downlink when it is known.
        uint32_t hz = sat::kDefaultAntennaHz;
        if (const PassRow *row = selected_pass()) {
            const uint32_t known = sat::downlink_hz(sats_[static_cast<std::size_t>(row->sat)].name);
            if (known != 0) hz = known;
        }
        char line[48];
        std::snprintf(line, sizeof(line), "antenna: %.1f mhz", hz / 1e6);
        if (popup_lines_[0]) lv_label_set_text(popup_lines_[0], line);
        std::snprintf(line, sizeof(line), "v-dipole, 2 x %.0f cm at 120 deg", location::dipole_leg_cm(hz));
        if (popup_lines_[1]) lv_label_set_text(popup_lines_[1], line);
        if (popup_lines_[2]) lv_label_set_text(popup_lines_[2], "flat, pointing north-south");
    }
    set_hidden(popup_.box, !on);
}

void UISatPage::load_location()
{
    last_location_ = lv_tick_get();
    const location::Position next = location::parse(app_support::read_file(location::file_path(app_support::home_dir())));
    if (next.valid != observer_.valid || next.lat != observer_.lat || next.lon != observer_.lon) {
        observer_ = next;
        want_compute_ = true;
    }
}

void UISatPage::stat_tle()
{
    last_stat_ = lv_tick_get();
    struct stat info {};
    tle_exists_ = stat(tle_path_.c_str(), &info) == 0;
    tle_mtime_ = tle_exists_ ? static_cast<long long>(info.st_mtime) : 0;
    if (tle_mtime_ != computed_mtime_) want_compute_ = true;
}

void UISatPage::start_worker()
{
    if (worker_busy_) return;
    if (worker_.joinable()) worker_.join();
    want_compute_ = false;
    computed_mtime_ = tle_mtime_;
    last_compute_ = unix_now();
    if (!observer_.valid) {
        outcome_ = Outcome::None;
        sats_.clear();
        passes_.clear();
        arc_.clear();
        return;
    }
    worker_busy_ = true;
    worker_stop_ = false;
    try {
        worker_ = std::thread(&UISatPage::worker_main, this, tle_path_, watch_path_, observer_of(observer_),
                              last_compute_);
    } catch (...) {
        worker_busy_ = false;
    }
}

// Runs on the worker thread: no LVGL calls, only the result fields under the mutex.
void UISatPage::worker_main(std::string tle_path, std::string watch_path, sat::Observer observer, double start)
{
    Outcome outcome = Outcome::NoTle;
    std::vector<sat::Tle> tracked;
    std::vector<PassRow> rows;
    try {
        const std::string text = app_support::read_file(tle_path);
        if (!text.empty()) {
            const std::vector<std::string> watch = sat::parse_watch(app_support::read_file(watch_path));
            for (const sat::Tle &tle : sat::parse_tle_file(text)) {
                if (tracked.size() >= kMaxTracked) break;
                if (sat::interesting(tle.name, watch)) tracked.push_back(tle);
            }
            outcome = tracked.empty() ? Outcome::NoMatch : Outcome::Ok;
            for (std::size_t i = 0; i < tracked.size() && !worker_stop_; ++i) {
                for (const sat::Pass &pass : sat::find_passes(tracked[i], observer, start, kSearchHours)) {
                    PassRow row;
                    row.sat = static_cast<int>(i);
                    row.pass = pass;
                    rows.push_back(row);
                }
            }
            std::sort(rows.begin(), rows.end(),
                      [](const PassRow &a, const PassRow &b) { return a.pass.aos < b.pass.aos; });
            if (rows.size() > kMaxPasses) rows.resize(kMaxPasses);
        }
    } catch (...) {
        outcome = Outcome::NoTle;
        tracked.clear();
        rows.clear();
    }
    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        result_outcome_ = outcome;
        result_sats_.swap(tracked);
        result_passes_.swap(rows);
        result_ready_ = !worker_stop_;
    }
    worker_busy_ = false;
}

void UISatPage::take_result()
{
    std::vector<sat::Tle> sats;
    std::vector<PassRow> passes;
    Outcome outcome = Outcome::None;
    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        if (!result_ready_) return;
        result_ready_ = false;
        outcome = result_outcome_;
        sats.swap(result_sats_);
        passes.swap(result_passes_);
    }
    // Keep the selection on the same pass when it is still in the list.
    int keep = 0;
    if (const PassRow *old = selected_pass()) {
        const int satnum = sats_[static_cast<std::size_t>(old->sat)].satnum;
        for (std::size_t i = 0; i < passes.size(); ++i) {
            if (sats[static_cast<std::size_t>(passes[i].sat)].satnum == satnum &&
                std::fabs(passes[i].pass.tmax - old->pass.tmax) < 300.0) {
                keep = static_cast<int>(i);
                break;
            }
        }
    }
    outcome_ = outcome;
    sats_.swap(sats);
    passes_.swap(passes);
    selected_ = keep;
    rebuild_arc();
}

void UISatPage::prune_passes(double now)
{
    bool removed = false;
    for (std::size_t i = 0; i < passes_.size();) {
        if (passes_[i].pass.los < now) {
            passes_.erase(passes_.begin() + static_cast<long>(i));
            if (selected_ > static_cast<int>(i)) --selected_;
            removed = true;
        } else {
            ++i;
        }
    }
    if (removed) {
        selected_ = std::clamp(selected_, 0, std::max(0, static_cast<int>(passes_.size()) - 1));
        rebuild_arc();
        want_compute_ = true;
    }
}

const UISatPage::PassRow *UISatPage::selected_pass() const
{
    if (selected_ < 0 || selected_ >= static_cast<int>(passes_.size())) return nullptr;
    const PassRow &row = passes_[static_cast<std::size_t>(selected_)];
    if (row.sat < 0 || row.sat >= static_cast<int>(sats_.size())) return nullptr;
    return &row;
}

void UISatPage::rebuild_arc()
{
    arc_.clear();
    const PassRow *row = selected_pass();
    if (!row || !observer_.valid) return;
    const sat::Tle &tle = sats_[static_cast<std::size_t>(row->sat)];
    const sat::Observer observer = observer_of(observer_);
    for (int i = 0; i <= kArcPoints; ++i) {
        ArcPoint point;
        point.time = row->pass.aos + (row->pass.los - row->pass.aos) * i / kArcPoints;
        const sat::Look look = sat::look_angles(tle, observer, point.time);
        point.azimuth_deg = look.azimuth_deg;
        point.elevation_deg = std::max(look.elevation_deg, 0.0);
        arc_.push_back(point);
    }
}

void UISatPage::start_update()
{
    if (update_pid_ > 0) {
        show_message("already updating");
        return;
    }
    if (!app_support::tool_present("curl")) {
        show_message("curl not installed");
        return;
    }
    const std::string directory = app_support::home_dir() + "/sat";
    if (mkdir(directory.c_str(), 0755) != 0 && errno != EEXIST) {
        show_message("cannot create ~/sat");
        return;
    }
    unlink(update_status_path_.c_str());
    update_pid_ = app_support::spawn(sat::update_command(tle_path_, update_status_path_));
    update_started_ = lv_tick_get();
    if (update_pid_ <= 0) show_message("cannot start curl");
}

void UISatPage::poll_update()
{
    const std::string status = app_support::read_file(update_status_path_);
    if (status.empty() && lv_tick_get() - update_started_ < kUpdateTimeoutMs) return;
    app_support::stop(update_pid_);
    unlink(update_status_path_.c_str());
    if (status.compare(0, 2, "ok") == 0) {
        show_message("tle updated");
        stat_tle();
        want_compute_ = true;
    } else {
        show_message(status.empty() ? "update timed out" : "update failed");
    }
}

void UISatPage::toggle_record()
{
    if (record_pid_ > 0) {
        finish_record("saved");
        return;
    }
    if (detect_pid_ > 0) return;
    const PassRow *row = selected_pass();
    if (!row || !observer_.valid) {
        show_message("no pass selected");
        return;
    }
    const sat::Tle &tle = sats_[static_cast<std::size_t>(row->sat)];
    const uint32_t hz = sat::downlink_hz(tle.name);
    if (hz == 0) {
        show_message("no known downlink");
        return;
    }
    if (sat::look_angles(tle, observer_of(observer_), unix_now()).elevation_deg <= 0.0) {
        show_message("below the horizon");
        return;
    }
    if (!app_support::tool_present("rtl_fm") || !app_support::tool_present("rtl_test")) {
        show_message("rtl-sdr not installed");
        return;
    }
    record_name_ = tle.name;
    record_hz_ = hz;
    record_until_ = row->pass.los;
    unlink(detect_path_.c_str());
    // The marker is written after rtl_test has exited and released the
    // dongle, so rtl_fm never starts while the probe still holds it.
    detect_pid_ = app_support::spawn("( " + sdr::detect_cmd() + "; echo " + kDetectDone + " ) > '" + detect_path_ +
                                     "' 2>&1");
    detect_started_ = lv_tick_get();
    show_message(detect_pid_ > 0 ? "checking dongle" : "cannot start rtl_test");
}

void UISatPage::poll_detect()
{
    const std::string output = app_support::read_file(detect_path_);
    const bool done = output.find(kDetectDone) != std::string::npos;
    if (!done && lv_tick_get() - detect_started_ < kDetectTimeoutMs) return;
    app_support::stop(detect_pid_);
    if (done && sdr::parse_detect(output).present)
        start_record();
    else
        show_message("no sdr");
}

void UISatPage::start_record()
{
    const std::string directory = app_support::home_dir() + "/sat";
    if (mkdir(directory.c_str(), 0755) != 0 && errno != EEXIST) {
        show_message("cannot create ~/sat");
        return;
    }
    record_path_ = directory + "/" + sat::record_file_name(record_name_, local_time(unix_now(), "%Y%m%d-%H%M%S"));
    std::FILE *file = std::fopen(record_path_.c_str(), "wb");
    if (!file) {
        show_message("cannot write recording");
        return;
    }
    // A header with no data yet; the sizes are filled in when recording ends.
    const std::vector<uint8_t> header = sat::wav_header(sat::kRecordRate, 0);
    std::fwrite(header.data(), 1, header.size(), file);
    std::fclose(file);
    unlink(record_status_path_.c_str());
    // rtl_fm appends raw samples after the header, to a regular file, so it
    // is never blocked on a pipe when it is signalled to stop. The status
    // file appears when it exits on its own.
    record_pid_ = app_support::spawn("( " + sat::record_command(record_hz_, record_path_) + "; echo exit > '" +
                                     record_status_path_ + "' )");
    if (record_pid_ <= 0) {
        unlink(record_path_.c_str());
        show_message("cannot start rtl_fm");
        return;
    }
    show_message("recording " + sat::short_name(record_name_, kNameChars));
}

void UISatPage::finish_record(const std::string &why)
{
    app_support::stop(record_pid_);
    unlink(record_status_path_.c_str());
    struct stat info {};
    const long long size = stat(record_path_.c_str(), &info) == 0 ? static_cast<long long>(info.st_size) : 0;
    if (size <= 44) {
        unlink(record_path_.c_str());
        if (!why.empty()) show_message("nothing recorded");
        return;
    }
    if (std::FILE *file = std::fopen(record_path_.c_str(), "r+b")) {
        const uint32_t data = static_cast<uint32_t>(std::min<long long>(size - 44, 0xFFFFFFFFll - 36));
        const std::vector<uint8_t> header = sat::wav_header(sat::kRecordRate, data);
        std::fwrite(header.data(), 1, header.size(), file);
        std::fclose(file);
    }
    if (!why.empty()) show_message(why + " " + record_path_.substr(record_path_.rfind('/') + 1));
}

void UISatPage::draw_sky(double now)
{
    if (!canvas_) return;
    const uint16_t bg = app_support::rgb565(cp0::theme::kPanel);
    const uint16_t grid = app_support::rgb565(cp0::theme::kLine);
    const uint16_t accent = app_support::rgb565(cp0::theme::kAccent);
    const uint16_t dim = app_support::rgb565(cp0::theme::kDim);
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
    // North up, east right: the horizon is the outer circle, the zenith the centre.
    auto plot_x = [](double azimuth_deg, double elevation_deg) {
        return kCentreX + kRadius * (1.0 - elevation_deg / 90.0) * std::sin(azimuth_deg * kPi / 180.0);
    };
    auto plot_y = [](double azimuth_deg, double elevation_deg) {
        return kCentreY - kRadius * (1.0 - elevation_deg / 90.0) * std::cos(azimuth_deg * kPi / 180.0);
    };

    std::fill(pixels_.begin(), pixels_.end(), bg);
    for (int r = kRadius / 3; r <= kRadius; r += kRadius / 3) circle(kCentreX, kCentreY, r, grid, 1);
    for (int d = -kRadius; d <= kRadius; d += 2) {
        put(kCentreX + d, kCentreY, grid);
        put(kCentreX, kCentreY + d, grid);
    }

    for (std::size_t i = 1; i < arc_.size(); ++i) {
        const ArcPoint &a = arc_[i - 1];
        const ArcPoint &b = arc_[i];
        line(plot_x(a.azimuth_deg, a.elevation_deg), plot_y(a.azimuth_deg, a.elevation_deg),
             plot_x(b.azimuth_deg, b.elevation_deg), plot_y(b.azimuth_deg, b.elevation_deg),
             b.time <= now ? accent : dim);
    }

    const PassRow *row = selected_pass();
    if (row && observer_.valid) {
        const sat::Look look =
            sat::look_angles(sats_[static_cast<std::size_t>(row->sat)], observer_of(observer_), now);
        if (look.elevation_deg > 0.0) {
            const int x = static_cast<int>(std::lround(plot_x(look.azimuth_deg, look.elevation_deg)));
            const int y = static_cast<int>(std::lround(plot_y(look.azimuth_deg, look.elevation_deg)));
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) put(x + dx, y + dy, text);
            if (blink_) circle(x, y, 5, text, 2);
        }
    }
    lv_obj_invalidate(canvas_);
}

void UISatPage::render(double now)
{
    const PassRow *chosen = selected_pass();
    const bool recording = record_pid_ > 0;

    if (title_) {
        std::string text = "sat  ";
        text += tle_exists_ ? "tle " + sat::format_age(now - static_cast<double>(tle_mtime_)) : "no tle";
        cp0::ui::set_text(title_, text.c_str(), cp0::theme::kText);
    }
    if (status_) {
        std::string text;
        uint32_t color = cp0::theme::kIdle;
        if (lv_tick_get() < message_until_) {
            text = message_;
            color = cp0::theme::kText;
        } else if (recording) {
            text = "rec " + sat::short_name(record_name_, kNameChars) + "  " + sat::format_mhz(record_hz_);
            color = cp0::theme::kDanger;
        } else if (update_pid_ > 0) {
            text = "updating tle";
            color = cp0::theme::kWarn;
        } else if (!observer_.valid) {
            text = "no location";
            color = cp0::theme::kWarn;
        } else if (chosen) {
            const std::string &name = sats_[static_cast<std::size_t>(chosen->sat)].name;
            const uint32_t hz = sat::downlink_hz(name);
            text = sat::short_name(name, kNameChars);
            if (hz != 0) text += "  " + sat::format_mhz(hz);
            color = cp0::theme::kAccent;
        } else if (worker_busy_) {
            text = "computing";
        }
        cp0::ui::set_text(status_, text.c_str(), color);
    }

    if (chosen) {
        const bool in_pass = now >= chosen->pass.aos;
        char caption[40];
        if (in_pass) {
            const sat::Look look =
                sat::look_angles(sats_[static_cast<std::size_t>(chosen->sat)], observer_of(observer_), now);
            std::snprintf(caption, sizeof(caption), "in pass  el %d", static_cast<int>(std::lround(look.elevation_deg)));
        } else {
            std::snprintf(caption, sizeof(caption), "%s  max el %d", selected_ == 0 ? "next pass" : "pass",
                          static_cast<int>(std::lround(chosen->pass.max_elevation_deg)));
        }
        cp0::ui::set_text(next_.label, caption, cp0::theme::kDim);
        cp0::ui::set_readout(next_, sat::format_countdown((in_pass ? chosen->pass.los : chosen->pass.aos) - now),
                             in_pass ? "to los" : "to aos", cp0::theme::kAccent);
    } else {
        cp0::ui::set_text(next_.label, "next pass", cp0::theme::kDim);
        cp0::ui::set_readout(next_, "--:--", "", cp0::theme::kIdle);
    }

    const int count = static_cast<int>(passes_.size());
    scroll_ = std::clamp(scroll_, 0, std::max(0, count - kRows));
    if (chosen) {
        if (selected_ < scroll_) scroll_ = selected_;
        if (selected_ >= scroll_ + kRows) scroll_ = selected_ - kRows + 1;
    }
    for (int i = 0; i < kRows; ++i) {
        const int index = scroll_ + i;
        std::string text;
        if (index < count) {
            const PassRow &row = passes_[static_cast<std::size_t>(index)];
            const std::string name =
                row.sat < static_cast<int>(sats_.size()) ? sats_[static_cast<std::size_t>(row.sat)].name : "";
            char line[48];
            std::snprintf(line, sizeof(line), "%-12.12s%5s%3d%4s", sat::short_name(name, kNameChars).c_str(),
                          local_time(row.pass.aos, "%H:%M").c_str(),
                          static_cast<int>(std::lround(row.pass.max_elevation_deg)),
                          sat::format_duration(row.pass.los - row.pass.aos).c_str());
            text = line;
        }
        cp0::ui::set_text(row_labels_[i], text.c_str(),
                          chosen && index == selected_ ? cp0::theme::kAccent : cp0::theme::kText);
    }
    if (select_bar_) {
        set_hidden(select_bar_, !chosen);
        const int y = 18 + (selected_ - scroll_) * 13;
        if (chosen && lv_obj_get_y(select_bar_) != y) lv_obj_set_y(select_bar_, y);
    }

    if (empty_) {
        std::string text;
        if (count == 0) {
            if (!observer_.valid)
                text = std::string("no location. set it:\n") + location::set_hint();
            else if (!tle_exists_ || outcome_ == Outcome::NoTle)
                text = worker_busy_ && tle_exists_ ? "computing passes" : "no tle: press u";
            else if (worker_busy_ || outcome_ == Outcome::None)
                text = "computing passes";
            else if (outcome_ == Outcome::NoMatch)
                text = "no tracked satellites in the tle file. add names to ~/sat/watch.txt";
            else
                text = "no passes in the next 24 h";
        }
        cp0::ui::set_text(empty_, text.c_str(), cp0::theme::kIdle);
    }

    cp0::ui::set_footer(hint_, {{"ent", recording ? "stop" : "record"}, {"up/dn", "pass"}, {"u", "tle"}, {"a", "antenna"}});
}

void UISatPage::timer_cb(lv_timer_t *timer) noexcept
{
    auto *page = static_cast<UISatPage *>(lv_timer_get_user_data(timer));
    if (!page) return;
    try {
        page->on_timer();
    } catch (...) {
    }
}

void UISatPage::on_timer()
{
    const uint32_t tick = lv_tick_get();
    const double now = unix_now();
    if (popup_until_ != 0 && tick >= popup_until_) show_popup(false);
    if (message_until_ != 0 && tick >= message_until_) message_until_ = 0;
    if (tick - last_location_ >= kLocationEveryMs) load_location();
    if (tick - last_stat_ >= kStatEveryMs) stat_tle();

    if (update_pid_ > 0) poll_update();
    if (detect_pid_ > 0) poll_detect();
    if (record_pid_ > 0) {
        if (!app_support::read_file(record_status_path_).empty())
            finish_record("stopped,");
        else if (now >= record_until_)
            finish_record("saved");
    }

    take_result();
    prune_passes(now);
    if (now - last_compute_ >= kRecomputeEveryS) want_compute_ = true;
    if (want_compute_ && !worker_busy_) start_worker();

    render(now);
    if (tick - last_second_ >= 500) {
        last_second_ = tick;
        blink_ = !blink_;
        draw_sky(now);
    }
}

void UISatPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UISatPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item) return;
    const uint32_t key = item->semantic_key ? item->semantic_key : item->key_code;
    try {
        page->handle_key(key, item->key_state == KBD_KEY_REPEATED);
    } catch (...) {
    }
}

void UISatPage::handle_key(uint32_t key, bool repeat)
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
        const int count = static_cast<int>(passes_.size());
        if (count == 0) return;
        const int next = std::clamp(selected_ + (key == KEY_DOWN ? 1 : -1), 0, count - 1);
        if (next != selected_) {
            selected_ = next;
            rebuild_arc();
        }
        const double now = unix_now();
        render(now);
        draw_sky(now);
        return;
    }
    if (repeat) return;
    if (key == KEY_A) {
        show_popup(true);
    } else if (key == KEY_U) {
        start_update();
    } else if (key == KEY_ENTER || key == KEY_KPENTER) {
        toggle_record();
    }
}

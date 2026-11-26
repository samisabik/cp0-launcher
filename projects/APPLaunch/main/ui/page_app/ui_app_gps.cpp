/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_gps.hpp"

#include "../app_support.hpp"
#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "input_keys.h"

#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

namespace {

namespace theme = cp0::theme;

// The primary UART on the 40-pin header, which the Grove port shares.
constexpr const char *kPort = "/dev/serial0";
constexpr const char *kGrovePower = "/sys/class/leds/grove_5v_out/brightness";
// 0 routes the Grove port to the UART, 1 to the I2C bus.
constexpr const char *kGroveFunction = "/sys/class/leds/grove_fun/brightness";

constexpr uint32_t kRefreshMs = 40;
// Shortest report interval the AT6668 accepts; a shorter request leaves it here.
constexpr int kFastIntervalMs = 100;
constexpr int kDefaultIntervalMs = 1000;
constexpr float kFastEnoughHz = 8.0f;
constexpr uint32_t kSilentMs = 2500;
constexpr uint32_t kRetryMs = 3000;
constexpr uint32_t kFlushMs = 1000;

void write_text(const char *path, const char *text)
{
    if (std::FILE *file = std::fopen(path, "w")) {
        std::fputs(text, file);
        std::fclose(file);
    }
}

std::string fixed(double value, int decimals)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
}

std::string elapsed(uint32_t seconds)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%u:%02u", static_cast<unsigned>(seconds / 60),
                  static_cast<unsigned>(seconds % 60));
    return text;
}

} // namespace

UIGpsPage::UIGpsPage() : AppPage()
{
    set_page_title("GPS");
    reclaim_top_bar();
    dir_ = app_support::home_dir() + "/gps";
    mkdir(dir_.c_str(), 0755);
    write_text(kGroveFunction, "0");
    write_text(kGrovePower, "1");
    window_start_ms_ = lv_tick_get();
    open_port();
    create_ui();
    render();
    timer_ = lv_timer_create(timer_cb, kRefreshMs, this);
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

UIGpsPage::~UIGpsPage()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
    try {
        stop_log(nullptr);
        if (fd_ >= 0) {
            request_rate(kDefaultIntervalMs);
            tcdrain(fd_);
        }
        close_port();
    } catch (...) {
    }
}

void UIGpsPage::create_ui()
{
    if (!ui_APP_Container) return;
    cp0::ui::paint_background(ui_APP_Container);
    const cp0::ui::TitleRow title = cp0::ui::title_row(ui_APP_Container, "gps");
    status_ = title.status;

    lat_ = cp0::ui::readout(ui_APP_Container, 4, 19, 154, 40, "lat");
    lon_ = cp0::ui::readout(ui_APP_Container, 162, 19, 154, 40, "lon");
    speed_ = cp0::ui::readout(ui_APP_Container, 4, 62, 102, 40, "speed");
    alt_ = cp0::ui::readout(ui_APP_Container, 110, 62, 102, 40, "alt");
    sats_ = cp0::ui::readout(ui_APP_Container, 216, 62, 100, 40, "sats");

    cp0::ui::panel(ui_APP_Container, 4, 105, 312, 46);
    for (int i = 0; i < kInfoRows; ++i) {
        info_[i] = cp0::ui::label(ui_APP_Container, 9, 108 + i * 13, theme::kIdle);
        if (!info_[i]) continue;
        lv_obj_set_width(info_[i], 302);
        lv_label_set_long_mode(info_[i], LV_LABEL_LONG_CLIP);
    }

    footer_ = cp0::ui::footer(ui_APP_Container);
}

void UIGpsPage::open_port()
{
    last_open_ms_ = lv_tick_get();
    fd_ = open(kPort, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) return;
    termios tty{};
    if (tcgetattr(fd_, &tty) != 0) {
        close_port();
        return;
    }
    cfmakeraw(&tty);
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS);
    if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
        close_port();
        return;
    }
    tcflush(fd_, TCIFLUSH);
    request_rate(kFastIntervalMs);
}

void UIGpsPage::close_port()
{
    if (fd_ < 0) return;
    close(fd_);
    fd_ = -1;
}

void UIGpsPage::request_rate(int interval_ms)
{
    if (fd_ < 0) return;
    const std::string command = gps::rate_command(interval_ms);
    const ssize_t written = write(fd_, command.data(), command.size());
    (void)written;
    last_rate_request_ms_ = lv_tick_get();
}

void UIGpsPage::read_port()
{
    if (fd_ < 0) return;
    char buffer[2048];
    while (true) {
        const ssize_t count = read(fd_, buffer, sizeof(buffer));
        if (count > 0) {
            last_data_ms_ = lv_tick_get();
            for (const gps::Fix &fix : parser_.feed(buffer, static_cast<std::size_t>(count))) take(fix);
            continue;
        }
        if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) close_port();
        return;
    }
}

void UIGpsPage::take(const gps::Fix &fix)
{
    fix_ = fix;
    have_report_ = true;
    ++window_reports_;
    if (!log_ || !fix.valid) return;
    if (std::fputs(gps::csv_row(fix).c_str(), log_) < 0) {
        stop_log("write failed");
        return;
    }
    ++log_rows_;
}

bool UIGpsPage::receiving() const
{
    return fd_ >= 0 && have_report_ && lv_tick_get() - last_data_ms_ < kSilentMs;
}

void UIGpsPage::start_log()
{
    if (log_) return;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    log_file_ = gps::log_name(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
                              local.tm_sec);
    log_ = std::fopen((dir_ + "/" + log_file_).c_str(), "w");
    if (!log_) {
        message_ = "cannot write in ~/gps";
        return;
    }
    std::fputs(gps::kCsvHeader, log_);
    log_rows_ = 0;
    log_started_ms_ = log_flushed_ms_ = lv_tick_get();
    message_.clear();
}

void UIGpsPage::stop_log(const char *reason)
{
    if (!log_) return;
    const bool saved = std::fclose(log_) == 0;
    log_ = nullptr;
    if (reason)
        message_ = std::string(reason) + ", " + std::to_string(log_rows_) + " rows in " + log_file_;
    else if (saved)
        message_ = "saved " + log_file_ + "  " + std::to_string(log_rows_) + " rows";
    else
        message_ = "could not save " + log_file_;
}

void UIGpsPage::render()
{
    const bool live = receiving();
    const bool fixed_position = live && fix_.valid;
    const uint32_t value_color = fixed_position ? theme::kText : theme::kDim;

    cp0::ui::set_readout(lat_, fixed_position ? fixed(fix_.lat, 6) : "--", "", value_color);
    cp0::ui::set_readout(lon_, fixed_position ? fixed(fix_.lon, 6) : "--", "", value_color);
    cp0::ui::set_readout(speed_, fixed_position && fix_.has_motion ? fixed(fix_.speed_kmh, 1) : "--", "km/h",
                         value_color);
    cp0::ui::set_readout(alt_, fixed_position ? fixed(fix_.alt_m, 1) : "--", "m", value_color);
    if (fixed_position)
        cp0::ui::set_readout(sats_, std::to_string(fix_.sats), "hdop " + fixed(fix_.hdop, 1));
    else
        cp0::ui::set_readout(sats_, live ? std::to_string(parser_.in_view()) : "--", "in view", theme::kDim);

    if (fd_ < 0) {
        cp0::ui::set_text(status_, "no serial port", theme::kDanger);
        cp0::ui::set_text(info_[0], "cannot open /dev/serial0", theme::kDanger);
    } else if (!live) {
        cp0::ui::set_text(status_, "no gps", theme::kDanger);
        cp0::ui::set_text(info_[0], "waiting for the receiver on the grove port", theme::kIdle);
    } else {
        if (log_)
            cp0::ui::set_text(status_, "rec", theme::kDanger);
        else if (fix_.valid)
            cp0::ui::set_text(status_, "fix", theme::kAccent);
        else
            cp0::ui::set_text(status_, "no fix", theme::kWarn);
        std::string line = "utc " + (fix_.time.empty() ? std::string("--") : fix_.time) + "  " + fixed(rate_hz_, 0) +
                           " Hz";
        if (fix_.valid && fix_.has_motion) line += "  course " + fixed(fix_.course_deg, 0);
        cp0::ui::set_text(info_[0], line.c_str(), theme::kText);
    }

    if (log_) {
        const std::string line = "log " + log_file_;
        cp0::ui::set_text(info_[1], line.c_str(), theme::kAccent);
        const std::string count = std::to_string(log_rows_) + " rows  " +
                                  elapsed((lv_tick_get() - log_started_ms_) / 1000) +
                                  (fixed_position ? "" : "  waiting for a fix");
        cp0::ui::set_text(info_[2], count.c_str(), fixed_position ? theme::kText : theme::kWarn);
    } else {
        cp0::ui::set_text(info_[1], "logs are csv files in ~/gps", theme::kIdle);
        cp0::ui::set_text(info_[2], message_.c_str(), theme::kText);
    }

    cp0::ui::set_footer(footer_, {{"enter", log_ ? "stop and save" : "start log"}});
}

void UIGpsPage::on_timer()
{
    const uint32_t now = lv_tick_get();
    if (fd_ < 0 && now - last_open_ms_ > kRetryMs) open_port();
    read_port();

    if (now - window_start_ms_ >= 1000) {
        rate_hz_ = static_cast<float>(window_reports_) * 1000.0f / static_cast<float>(now - window_start_ms_);
        window_reports_ = 0;
        window_start_ms_ = now;
        // A receiver that was powered after the page opened missed the request.
        if (receiving() && rate_hz_ < kFastEnoughHz && now - last_rate_request_ms_ > kRetryMs)
            request_rate(kFastIntervalMs);
    }
    if (log_ && now - log_flushed_ms_ >= kFlushMs) {
        log_flushed_ms_ = now;
        if (std::fflush(log_) != 0) stop_log("write failed");
    }
    render();
}

void UIGpsPage::timer_cb(lv_timer_t *timer) noexcept
{
    auto *page = static_cast<UIGpsPage *>(lv_timer_get_user_data(timer));
    if (!page) return;
    try {
        page->on_timer();
    } catch (...) {
    }
}

void UIGpsPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UIGpsPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item || item->key_state == KBD_KEY_REPEATED) return;
    const uint32_t key = item->semantic_key ? item->semantic_key : item->key_code;
    try {
        page->handle_key(key);
    } catch (...) {
    }
}

void UIGpsPage::handle_key(uint32_t key)
{
    switch (key) {
    case KEY_ESC:
        stop_log(nullptr);
        if (navigate_home) navigate_home();
        return;
    case KEY_ENTER:
    case KEY_KPENTER:
    case KEY_SPACE:
        if (log_)
            stop_log(nullptr);
        else
            start_log();
        break;
    default:
        return;
    }
    render();
}

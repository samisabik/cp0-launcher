/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_pins.hpp"

#include "../app_support.hpp"
#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "input_keys.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <system_error>
#include <unistd.h>

#if __has_include(<linux/gpio.h>) && __has_include(<linux/i2c-dev.h>)
#include <linux/gpio.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#define CP0_PINS_HAS_LINUX 1
#else
#define CP0_PINS_HAS_LINUX 0
#endif

namespace {

constexpr uint32_t kRefreshMs = 66;
constexpr uint32_t kMessageMs = 3000;
constexpr int kViewTop = cp0::theme::kBodyTop;
constexpr std::size_t kDecodeColumns = 42;
constexpr const char *kLinesConfigKey = "pins_lines";
constexpr const char *kI2cConfigKey = "pins_i2c";

int64_t monotonic_ns()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
}

std::string config_string(const char *key)
{
    std::string value;
    cp0_signal_config_api({"GetStr", key, ""}, [&](int code, std::string data) {
        if (code == 0) value = std::move(data);
    });
    return value;
}

void show(lv_obj_t *obj, bool on)
{
    if (!obj) return;
    if (on)
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *view(lv_obj_t *parent)
{
    lv_obj_t *obj = cp0::ui::box(parent, 0, kViewTop, cp0::theme::kScreenW, cp0::theme::kBodyH, cp0::theme::kBg);
    if (obj) lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    return obj;
}

lv_obj_t *line_label(lv_obj_t *parent, int x, int y, uint32_t color)
{
    lv_obj_t *obj = cp0::ui::label(parent, x, y, color);
    if (obj) {
        lv_obj_set_width(obj, 302);
        lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
    }
    return obj;
}

void fill(std::vector<uint16_t> &pixels, int stride, int x, int y, int w, int h, uint16_t color)
{
    for (int row = y; row < y + h; ++row)
        std::fill_n(&pixels[static_cast<std::size_t>(row) * stride + x], w, color);
}

#if CP0_PINS_HAS_LINUX
constexpr int kMaxChips = 8;
constexpr int kLogicPollMs = 20;
constexpr std::size_t kMaxEdges = 20000;
constexpr std::size_t kFreeListChars = 84;

std::string error_text(int code)
{
    return cp0::ui::lower(std::generic_category().message(code));
}
#endif

} // namespace

UIPinsPage::UIPinsPage() : AppPage()
{
    set_page_title("Pins");
    reclaim_top_bar();
    grid_pixels_.assign(static_cast<std::size_t>(kGridW) * kGridH, app_support::rgb565(cp0::theme::kPanel));
    trace_pixels_.assign(static_cast<std::size_t>(kTraceW) * kTraceH, app_support::rgb565(cp0::theme::kPanel));
    find_buses();
    load_lines();
    if (lines_.empty()) describe_chips();
    create_ui();
    draw_grid();
    draw_traces();
    render();
    timer_ = lv_timer_create(timer_cb, kRefreshMs, this);
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

UIPinsPage::~UIPinsPage()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
    stop_scan();
    logic_stop_ = true;
    if (logic_thread_.joinable()) logic_thread_.join();
}

void UIPinsPage::create_ui()
{
    if (!ui_APP_Container) return;
    cp0::ui::paint_background(ui_APP_Container);
    const cp0::ui::TitleRow row = cp0::ui::title_row(ui_APP_Container, "pins");
    status_ = row.status;
    create_i2c_view();
    create_logic_view();
    hint_ = cp0::ui::footer(ui_APP_Container);
}

void UIPinsPage::create_i2c_view()
{
    i2c_view_ = view(ui_APP_Container);
    if (!i2c_view_) return;
    cp0::ui::panel(i2c_view_, 4, 19 - kViewTop, 312, 132);
    bus_label_ = line_label(i2c_view_, 9, 22 - kViewTop, cp0::theme::kText);

    const int grid_x = 30, grid_y = 46 - kViewTop;
    char text[4];
    for (int column = 0; column < pins::kGridColumns; ++column) {
        lv_obj_t *head = cp0::ui::label(i2c_view_, grid_x + column * kCellW + 5, grid_y - 12, cp0::theme::kDim);
        std::snprintf(text, sizeof(text), "%x", column);
        if (head) lv_label_set_text(head, text);
    }
    for (int row = 0; row < pins::kGridRows; ++row) {
        lv_obj_t *head = cp0::ui::label(i2c_view_, 9, grid_y + row * kCellH - 1, cp0::theme::kDim);
        std::snprintf(text, sizeof(text), "%x0", row);
        if (head) lv_label_set_text(head, text);
    }
    grid_canvas_ = lv_canvas_create(i2c_view_);
    if (grid_canvas_) {
        lv_canvas_set_buffer(grid_canvas_, grid_pixels_.data(), kGridW, kGridH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(grid_canvas_, grid_x, grid_y);
    }
    found_label_ = line_label(i2c_view_, 9, 127 - kViewTop, cp0::theme::kText);
    scan_label_ = line_label(i2c_view_, 9, 139 - kViewTop, cp0::theme::kDim);
}

void UIPinsPage::create_logic_view()
{
    logic_view_ = view(ui_APP_Container);
    if (!logic_view_) return;
    cp0::ui::panel(logic_view_, 4, 19 - kViewTop, 312, 132);
    trace_canvas_ = lv_canvas_create(logic_view_);
    if (trace_canvas_) {
        lv_canvas_set_buffer(trace_canvas_, trace_pixels_.data(), kTraceW, kTraceH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(trace_canvas_, 36, 22 - kViewTop);
    }
    for (std::size_t i = 0; i < pins::kMaxLines; ++i)
        line_labels_[i] = cp0::ui::label(logic_view_, 9, 24 - kViewTop + static_cast<int>(i) * kTracePitch,
                                         cp0::theme::kIdle);
    decode_label_ = line_label(logic_view_, 9, 110 - kViewTop, cp0::theme::kText);
    error_label_ = line_label(logic_view_, 9, 123 - kViewTop, cp0::theme::kDanger);
    info_label_ = line_label(logic_view_, 9, 137 - kViewTop, cp0::theme::kDim);

    help_head_ = line_label(logic_view_, 9, 22 - kViewTop, cp0::theme::kText);
    help_body_ = cp0::ui::label(logic_view_, 9, 35 - kViewTop, cp0::theme::kIdle);
    if (help_body_) {
        lv_obj_set_size(help_body_, 302, 114);
        lv_label_set_long_mode(help_body_, LV_LABEL_LONG_WRAP);
    }
}

void UIPinsPage::show_message(const std::string &text)
{
    message_ = text;
    message_until_ = lv_tick_get() + kMessageMs;
    render();
}

void UIPinsPage::find_buses()
{
    buses_.clear();
    DIR *dir = opendir("/dev");
    if (!dir) return;
    while (const dirent *entry = readdir(dir)) {
        int bus = -1;
        char tail = 0;
        if (std::sscanf(entry->d_name, "i2c-%d%c", &bus, &tail) == 1 && bus >= 0) buses_.push_back(bus);
    }
    closedir(dir);
    std::sort(buses_.begin(), buses_.end());
}

void UIPinsPage::start_scan()
{
    if (scan_running_ || buses_.empty()) return;
    stop_scan();
    probes_.fill(pins::Probe::Unknown);
    scan_error_.clear();
    {
        std::lock_guard<std::mutex> lock(scan_mutex_);
        shared_probes_ = probes_;
        shared_scan_error_.clear();
    }
    scanned_ = true;
    scan_stop_ = false;
    scan_running_ = true;
    scan_was_running_ = true;
    const int bus = buses_[static_cast<std::size_t>(bus_index_)];
    scan_thread_ = std::thread([this, bus] { scan_loop(bus); });
}

void UIPinsPage::stop_scan()
{
    scan_stop_ = true;
    if (scan_thread_.joinable()) scan_thread_.join();
}

void UIPinsPage::scan_loop(int bus)
{
#if CP0_PINS_HAS_LINUX
    const std::string name = "i2c-" + std::to_string(bus);
    const int fd = ::open(("/dev/" + name).c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        const std::string error = name + ": " + error_text(errno);
        std::lock_guard<std::mutex> lock(scan_mutex_);
        shared_scan_error_ = error;
        scan_running_ = false;
        return;
    }
    for (int address = pins::kI2cFirst; address <= pins::kI2cLast && !scan_stop_; ++address) {
        scan_address_ = address;
        pins::Probe result = pins::Probe::Empty;
        // linux/i2c-dev.h says only I2C_SLAVE_FORCE takes an address that a
        // driver already uses, so a refused I2C_SLAVE is shown as in use and
        // left alone. That the refusal is EBUSY is unverified.
        if (ioctl(fd, I2C_SLAVE, static_cast<unsigned long>(address)) < 0) {
            if (errno == EBUSY) result = pins::Probe::InUse;
        } else {
            // A read is the only probe: the address is sent with the read bit
            // and one byte is clocked in. Nothing is ever written.
            uint8_t byte = 0;
            if (::read(fd, &byte, 1) == 1) result = pins::Probe::Found;
        }
        std::lock_guard<std::mutex> lock(scan_mutex_);
        shared_probes_[static_cast<std::size_t>(address)] = result;
    }
    ::close(fd);
#else
    (void)bus;
    {
        std::lock_guard<std::mutex> lock(scan_mutex_);
        shared_scan_error_ = "i2c-dev is not available in this build";
    }
#endif
    scan_running_ = false;
}

void UIPinsPage::draw_grid()
{
    if (grid_pixels_.empty()) return;
    const uint16_t bg = app_support::rgb565(cp0::theme::kPanel);
    const uint16_t dot = app_support::rgb565(cp0::theme::kBracket);
    const uint16_t dash = app_support::rgb565(cp0::theme::kDim);
    const uint16_t found = app_support::rgb565(cp0::theme::kAccent);
    const uint16_t in_use = app_support::rgb565(cp0::theme::kWarn);
    const uint16_t cursor = app_support::rgb565(cp0::theme::kText);
    std::fill(grid_pixels_.begin(), grid_pixels_.end(), bg);
    const int probing = scan_running_ ? scan_address_.load() : -1;
    for (int row = 0; row < pins::kGridRows; ++row) {
        for (int column = 0; column < pins::kGridColumns; ++column) {
            const int address = pins::grid_address(row, column);
            if (address < 0) continue;
            const int x = column * kCellW, y = row * kCellH;
            switch (probes_[static_cast<std::size_t>(address)]) {
            case pins::Probe::Found:
                fill(grid_pixels_, kGridW, x + 2, y + 1, 13, 7, found);
                break;
            case pins::Probe::InUse:
                // Hollow, so it differs from a found device by shape too.
                fill(grid_pixels_, kGridW, x + 2, y + 1, 13, 7, in_use);
                fill(grid_pixels_, kGridW, x + 3, y + 2, 11, 5, bg);
                break;
            case pins::Probe::Empty:
                fill(grid_pixels_, kGridW, x + 6, y + 4, 5, 1, dash);
                break;
            case pins::Probe::Unknown:
                fill(grid_pixels_, kGridW, x + 8, y + 4, 1, 1, address == probing ? cursor : dot);
                break;
            }
        }
    }
    if (grid_canvas_) lv_obj_invalidate(grid_canvas_);
}

void UIPinsPage::load_lines()
{
    lines_ = pins::parse_lines(config_string(kLinesConfigKey));
    if (!pins::parse_i2c_pair(config_string(kI2cConfigKey), lines_.size(), i2c_scl_, i2c_sda_))
        i2c_scl_ = i2c_sda_ = -1;
}

void UIPinsPage::describe_chips()
{
    chip_help_.clear();
#if CP0_PINS_HAS_LINUX
    std::string chips, free_lines;
    bool listed = false;
    int first_errno = 0;
    for (int index = 0; index < kMaxChips; ++index) {
        const std::string name = "gpiochip" + std::to_string(index);
        const int fd = ::open(("/dev/" + name).c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            if (first_errno == 0 || errno == EACCES) first_errno = errno;
            continue;
        }
        gpiochip_info info{};
        if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0) {
            chips += (chips.empty() ? "" : "  ") + name + " " + std::to_string(info.lines);
            for (uint32_t offset = 0; !listed && offset < info.lines; ++offset) {
                gpio_v2_line_info line{};
                line.offset = offset;
                if (ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &line) != 0) continue;
                if (line.flags & GPIO_V2_LINE_FLAG_USED) continue;
                line.name[sizeof(line.name) - 1] = '\0';
                std::string item = std::to_string(offset);
                if (line.name[0]) item += std::string(":") + line.name;
                if (free_lines.size() + item.size() + 1 > kFreeListChars) {
                    free_lines += " ..";
                    break;
                }
                free_lines += (free_lines.empty() ? "" : " ") + item;
            }
            listed = true;
        }
        ::close(fd);
    }
    if (chips.empty()) {
        chip_help_ = "no gpio chip: " + error_text(first_errno ? first_errno : ENOENT);
        return;
    }
    chip_help_ = "lines: " + chips + "\nfree on the first: " + (free_lines.empty() ? "none" : free_lines);
#else
    chip_help_ = "gpio is not available in this build";
#endif
}

void UIPinsPage::arm()
{
    if (armed_ || lines_.empty()) return;
    if (logic_thread_.joinable()) logic_thread_.join();
    {
        std::lock_guard<std::mutex> lock(logic_mutex_);
        for (std::size_t i = 0; i < pins::kMaxLines; ++i) {
            edges_[i].clear();
            initial_[i] = false;
            line_error_[i].clear();
        }
    }
    lost_ = 0;
    clock_hz_ = 0;
    scroll_divs_ = 0;
    logic_stop_ = false;
    armed_ = true;
    captured_ = true;
    logic_thread_ = std::thread([this] { logic_loop(); });
}

void UIPinsPage::disarm()
{
    if (!armed_) return;
    logic_stop_ = true;
    if (logic_thread_.joinable()) logic_thread_.join();
    armed_ = false;
    stop_ns_ = monotonic_ns();
    logic_dirty_ = true;
}

void UIPinsPage::logic_loop()
{
#if CP0_PINS_HAS_LINUX
    pollfd fds[pins::kMaxLines];
    std::size_t owner[pins::kMaxLines];
    uint32_t next_seqno[pins::kMaxLines];
    bool seen[pins::kMaxLines];
    nfds_t open_count = 0;

    for (std::size_t i = 0; i < lines_.size(); ++i) {
        const pins::LineRef &line = lines_[i];
        std::string error;
        bool level = false;
        int line_fd = -1;
        const std::string chip_name = "gpiochip" + std::to_string(line.chip);
        const int chip = ::open(("/dev/" + chip_name).c_str(), O_RDWR | O_CLOEXEC);
        if (chip < 0) {
            error = chip_name + ": " + error_text(errno);
        } else {
            // Input with both-edge detection, nothing else: no output flag and
            // no bias change. Unverified on this board: asking for a pin that
            // is muxed to a bus controller may take it away from that bus.
            gpio_v2_line_request request{};
            request.offsets[0] = static_cast<uint32_t>(line.offset);
            request.num_lines = 1;
            std::snprintf(request.consumer, sizeof(request.consumer), "cp0-pins");
            request.config.flags =
                GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING;
            request.event_buffer_size = 4096;
            const int result = ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &request);
            const int code = errno;
            ::close(chip);
            if (result < 0) {
                error = std::to_string(line.offset) + ": " + error_text(code);
            } else {
                line_fd = request.fd;
                fcntl(line_fd, F_SETFL, fcntl(line_fd, F_GETFL) | O_NONBLOCK);
                gpio_v2_line_values values{};
                values.mask = 1;
                if (ioctl(line_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) == 0) level = (values.bits & 1) != 0;
            }
        }
        if (line_fd >= 0) {
            fds[open_count] = pollfd{line_fd, POLLIN, 0};
            owner[open_count] = i;
            next_seqno[open_count] = 0;
            seen[open_count] = false;
            ++open_count;
        }
        std::lock_guard<std::mutex> lock(logic_mutex_);
        initial_[i] = level;
        line_error_[i] = error;
    }

    gpio_v2_line_event events[64];
    std::vector<pins::Edge> batch;
    while (!logic_stop_) {
        if (open_count == 0 || poll(fds, open_count, kLogicPollMs) <= 0) {
            if (open_count == 0) poll(nullptr, 0, kLogicPollMs);
            continue;
        }
        for (nfds_t n = 0; n < open_count; ++n) {
            if (!(fds[n].revents & POLLIN)) continue;
            batch.clear();
            for (;;) {
                const ssize_t count = ::read(fds[n].fd, events, sizeof(events));
                if (count <= 0) break;
                for (ssize_t e = 0; e < count / static_cast<ssize_t>(sizeof(events[0])); ++e) {
                    // A jump in the per-line sequence number is counted as
                    // edges the kernel buffer dropped (unverified that this is
                    // the only cause of a jump).
                    if (seen[n] && events[e].line_seqno != next_seqno[n])
                        lost_ += events[e].line_seqno - next_seqno[n];
                    seen[n] = true;
                    next_seqno[n] = events[e].line_seqno + 1;
                    batch.push_back({static_cast<int64_t>(events[e].timestamp_ns),
                                     events[e].id == GPIO_V2_LINE_EVENT_RISING_EDGE});
                }
            }
            if (batch.empty()) continue;
            std::lock_guard<std::mutex> lock(logic_mutex_);
            std::vector<pins::Edge> &edges = edges_[owner[n]];
            edges.insert(edges.end(), batch.begin(), batch.end());
            pins::trim(edges, initial_[owner[n]], kMaxEdges);
        }
    }
    for (nfds_t n = 0; n < open_count; ++n) ::close(fds[n].fd);
#else
    std::lock_guard<std::mutex> lock(logic_mutex_);
    for (std::size_t i = 0; i < lines_.size(); ++i) line_error_[i] = "gpio is not available in this build";
#endif
}

void UIPinsPage::draw_traces()
{
    if (trace_pixels_.empty()) return;
    const uint16_t bg = app_support::rgb565(cp0::theme::kPanel);
    const uint16_t grid = app_support::rgb565(cp0::theme::kLine);
    const uint16_t bus = app_support::rgb565(cp0::theme::kAccent);
    const uint16_t plain = app_support::rgb565(i2c_scl_ >= 0 ? cp0::theme::kDim : cp0::theme::kAccent);
    std::fill(trace_pixels_.begin(), trace_pixels_.end(), bg);
    for (int x = kDivPx; x < kTraceW; x += kDivPx)
        for (int y = 0; y < kTraceH; y += 2) trace_pixels_[static_cast<std::size_t>(y) * kTraceW + x] = grid;

    const int64_t span = div_ns_ * kDivs;
    const int64_t end = armed_ ? monotonic_ns() : stop_ns_ - static_cast<int64_t>(scroll_divs_) * div_ns_;
    const int64_t start = end - span;

    std::array<std::vector<pins::Edge>, pins::kMaxLines> part;
    std::array<bool, pins::kMaxLines> level{};
    std::array<bool, pins::kMaxLines> failed{};
    std::string error;
    std::size_t total = 0;
    {
        std::lock_guard<std::mutex> lock(logic_mutex_);
        for (std::size_t i = 0; i < lines_.size(); ++i) {
            bool at_start = false;
            part[i] = pins::window(edges_[i], initial_[i], start, end, at_start);
            level[i] = at_start;
            total += edges_[i].size();
            failed[i] = !line_error_[i].empty();
            if (failed[i] && error.empty()) error = line_error_[i];
        }
    }

    for (std::size_t i = 0; captured_ && i < lines_.size(); ++i) {
        if (failed[i]) continue;
        const bool is_bus = static_cast<int>(i) == i2c_scl_ || static_cast<int>(i) == i2c_sda_;
        const uint16_t color = is_bus ? bus : plain;
        const int top = 2 + static_cast<int>(i) * kTracePitch;
        const std::vector<uint8_t> columns = pins::trace_columns(part[i], level[i], start, span, kTraceW);
        for (int x = 0; x < kTraceW; ++x) {
            const uint8_t value = columns[static_cast<std::size_t>(x)];
            if (value & pins::kColumnEdge)
                fill(trace_pixels_, kTraceW, x, top, 1, kTraceAmp + 1, color);
            else
                trace_pixels_[static_cast<std::size_t>(top + ((value & pins::kColumnHigh) ? 0 : kTraceAmp)) * kTraceW +
                              x] = color;
        }
    }
    if (trace_canvas_) lv_obj_invalidate(trace_canvas_);

    std::string decoded;
    if (captured_ && i2c_scl_ >= 0) {
        const std::size_t scl = static_cast<std::size_t>(i2c_scl_), sda = static_cast<std::size_t>(i2c_sda_);
        decoded = pins::format_i2c(pins::decode_i2c(part[scl], part[sda], level[scl], level[sda]));
        const uint32_t hz = pins::i2c_clock_hz(part[scl]);
        if (hz) clock_hz_ = hz;
        if (decoded.empty()) decoded = "no i2c traffic in view";
        if (decoded.size() > kDecodeColumns) decoded = ".." + decoded.substr(decoded.size() - (kDecodeColumns - 2));
    }
    cp0::ui::set_text(decode_label_, decoded.c_str(), cp0::theme::kText);
    cp0::ui::set_text(error_label_, error.c_str(), cp0::theme::kDanger);

    std::string info = pins::format_time(div_ns_) + "/div  ";
    info += armed_ ? "armed" : captured_ ? "stopped" : "enter to arm";
    if (captured_) info += "  " + std::to_string(total) + " edges";
    const uint32_t lost = lost_;
    if (lost) info += "  " + std::to_string(lost) + " lost";
    if (!armed_ && scroll_divs_ > 0) info += "  -" + std::to_string(scroll_divs_) + " div";
    cp0::ui::set_text(info_label_, info.c_str(), cp0::theme::kDim);

    for (std::size_t i = 0; i < pins::kMaxLines; ++i) {
        std::string name;
        if (i < lines_.size())
            name = static_cast<int>(i) == i2c_scl_   ? "scl"
                   : static_cast<int>(i) == i2c_sda_ ? "sda"
                                                     : std::to_string(lines_[i].offset);
        cp0::ui::set_text(line_labels_[i], name.c_str(), i < lines_.size() && failed[i] ? cp0::theme::kDanger
                                                                                        : cp0::theme::kIdle);
    }
    logic_dirty_ = false;
}

void UIPinsPage::set_tab(Tab tab)
{
    if (tab == tab_) return;
    if (tab_ == Tab::Logic) disarm();
    tab_ = tab;
    logic_dirty_ = true;
    render();
}

void UIPinsPage::render()
{
    const bool i2c = tab_ == Tab::I2c;
    const bool configured = !lines_.empty();
    show(i2c_view_, i2c);
    show(logic_view_, !i2c);
    show(trace_canvas_, configured);
    show(decode_label_, configured);
    show(error_label_, configured);
    show(info_label_, configured);
    for (lv_obj_t *label : line_labels_) show(label, configured);
    show(help_head_, !configured);
    show(help_body_, !configured);

    const bool scanning = scan_running_;
    const int found = pins::count(probes_, pins::Probe::Found);
    const int in_use = pins::count(probes_, pins::Probe::InUse);
    const std::string bus = buses_.empty() ? std::string() : "i2c-" + std::to_string(buses_[bus_index_]);
    char hex[8];
    std::snprintf(hex, sizeof(hex), "%02X", static_cast<unsigned>(scan_address_.load()));

    if (status_) {
        std::string text;
        uint32_t color = cp0::theme::kIdle;
        if (lv_tick_get() < message_until_) {
            text = message_;
            color = cp0::theme::kText;
        } else if (i2c) {
            if (buses_.empty()) {
                text = "no i2c bus";
                color = cp0::theme::kDanger;
            } else if (scanning) {
                text = bus + "  scan " + hex;
                color = cp0::theme::kWarn;
            } else if (scanned_ && scan_error_.empty()) {
                text = bus + "  " + std::to_string(found) + " found";
                color = cp0::theme::kAccent;
            } else {
                text = bus;
            }
        } else if (!configured) {
            text = "no lines";
        } else {
            if (i2c_scl_ >= 0) text = clock_hz_ ? "i2c  " + pins::format_hz(clock_hz_) : std::string("i2c");
            else text = std::to_string(lines_.size()) + (lines_.size() == 1 ? " line" : " lines");
            if (armed_) text += "  armed";
            color = armed_ ? cp0::theme::kDanger : clock_hz_ ? cp0::theme::kAccent : cp0::theme::kIdle;
        }
        cp0::ui::set_text(status_, text.c_str(), color);
    }

    if (i2c) {
        if (buses_.empty()) {
            cp0::ui::set_text(bus_label_, "no /dev/i2c-* nodes", cp0::theme::kDanger);
        } else {
            const std::string text = "bus  " + bus + "   " + std::to_string(bus_index_ + 1) + " of " +
                                     std::to_string(buses_.size());
            cp0::ui::set_text(bus_label_, text.c_str(), cp0::theme::kText);
        }
        std::string found_text = "enter scans 08..77 with reads, no writes";
        if (scanned_) found_text = "found  " + (found ? pins::found_list(probes_) : std::string("none"));
        cp0::ui::set_text(found_label_, found_text.c_str(), scanned_ ? cp0::theme::kText : cp0::theme::kDim);

        std::string state;
        uint32_t color = cp0::theme::kDim;
        if (!scan_error_.empty()) {
            state = scan_error_;
            color = cp0::theme::kDanger;
        } else if (scanning) {
            state = std::string("probing ") + hex;
        } else if (in_use) {
            state = std::to_string(in_use) + " in use by a kernel driver, not probed";
            color = cp0::theme::kWarn;
        }
        cp0::ui::set_text(scan_label_, state.c_str(), color);
        cp0::ui::set_footer(hint_, {{"< >", "bus"}, {"ent", "scan"}, {"tab", "logic"}});
        return;
    }

    if (!configured) {
        cp0::ui::set_text(help_head_, "no lines configured", cp0::theme::kText);
        const std::string body = "add to ~/.config/cardputerzero/config.json\n"
                                 "\"pins_lines\": \"5,6\"\n"
                                 "  line offsets, or chip:offset pairs\n"
                                 "\"pins_i2c\": \"0,1\"  scl,sda index\n"
                                 "inputs only. use spare pins.\n" +
                                 chip_help_;
        cp0::ui::set_text(help_body_, body.c_str(), cp0::theme::kIdle);
        cp0::ui::set_footer(hint_, {{"tab", "i2c"}});
        return;
    }
    cp0::ui::set_footer(hint_, {{"ent", armed_ ? "stop" : "arm"}, {"z", "zoom"}, {"< >", "scroll"}, {"tab", "i2c"}});
}

void UIPinsPage::timer_cb(lv_timer_t *timer) noexcept
{
    auto *page = static_cast<UIPinsPage *>(lv_timer_get_user_data(timer));
    if (!page) return;
    try {
        page->on_timer();
    } catch (...) {
    }
}

void UIPinsPage::on_timer()
{
    bool changed = false;
    if (scan_was_running_) {
        const bool running = scan_running_;
        {
            std::lock_guard<std::mutex> lock(scan_mutex_);
            probes_ = shared_probes_;
            scan_error_ = shared_scan_error_;
        }
        scan_was_running_ = running;
        if (tab_ == Tab::I2c) draw_grid();
        changed = true;
    }
    if (tab_ == Tab::Logic && !lines_.empty() && (armed_ || logic_dirty_)) {
        const uint32_t before = clock_hz_;
        draw_traces();
        changed = changed || before != clock_hz_;
    }
    if (message_until_ != 0 && lv_tick_get() >= message_until_) {
        message_until_ = 0;
        changed = true;
    }
    if (changed) render();
}

void UIPinsPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UIPinsPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item) return;
    const uint32_t key = item->semantic_key ? item->semantic_key : item->key_code;
    try {
        page->handle_key(key, item->key_state == KBD_KEY_REPEATED);
    } catch (...) {
    }
}

void UIPinsPage::handle_key(uint32_t key, bool repeat)
{
    if (repeat && key != KEY_LEFT && key != KEY_RIGHT) return;
    if (key == KEY_ESC) {
        if (navigate_home) navigate_home();
        return;
    }
    if (key == KEY_TAB) {
        set_tab(tab_ == Tab::I2c ? Tab::Logic : Tab::I2c);
        return;
    }
    const bool enter = key == KEY_ENTER || key == KEY_KPENTER;

    if (tab_ == Tab::I2c) {
        if ((key == KEY_LEFT || key == KEY_RIGHT) && buses_.size() > 1) {
            if (scan_running_) {
                show_message("scan running");
                return;
            }
            const int count = static_cast<int>(buses_.size());
            bus_index_ = (bus_index_ + (key == KEY_RIGHT ? 1 : count - 1)) % count;
            probes_.fill(pins::Probe::Unknown);
            scanned_ = false;
            scan_error_.clear();
            draw_grid();
            render();
        } else if (enter) {
            if (buses_.empty())
                show_message("no i2c bus");
            else if (!scan_running_) {
                start_scan();
                draw_grid();
                render();
            }
        }
        return;
    }

    if (lines_.empty()) return;
    if (enter) {
        if (armed_)
            disarm();
        else
            arm();
        logic_dirty_ = true;
        render();
    } else if (key == KEY_Z) {
        div_ns_ = div_ns_ <= pins::kMinDivNs ? pins::kMaxDivNs : pins::next_step(div_ns_, false);
        scroll_divs_ = 0;
        logic_dirty_ = true;
    } else if ((key == KEY_LEFT || key == KEY_RIGHT) && !armed_ && captured_) {
        scroll_divs_ = std::max(0, scroll_divs_ + (key == KEY_LEFT ? 1 : -1));
        logic_dirty_ = true;
    }
}

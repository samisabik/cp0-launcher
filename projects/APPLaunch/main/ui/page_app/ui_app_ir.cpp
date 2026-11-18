/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_ir.hpp"

#include "../app_support.hpp"
#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "input_keys.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

#if __has_include(<linux/lirc.h>)
#include <linux/lirc.h>
#include <sys/ioctl.h>
#define CP0_IR_HAS_LIRC 1
#else
#define CP0_IR_HAS_LIRC 0
#endif

namespace {

constexpr uint32_t kRefreshMs = 66;
constexpr uint32_t kMessageMs = 3000;
constexpr const char *kRemoteConfigKey = "ir_remote";

int config_remote()
{
    int value = 1;
    cp0_signal_config_api({"GetInt", kRemoteConfigKey, "1"}, [&](int code, std::string data) {
        if (code == 0) value = std::atoi(data.c_str());
    });
    return std::clamp(value, 1, ir::kRemoteCount);
}

void set_config_remote(int number)
{
    cp0_signal_config_api({"SetInt", kRemoteConfigKey, std::to_string(number)}, nullptr);
    cp0_signal_config_api({"Save"}, nullptr);
}

#if CP0_IR_HAS_LIRC
constexpr int kDeviceCount = 4;
constexpr int kPollMs = 40;
constexpr auto kRescanEvery = std::chrono::seconds(3);
// A frame with no closing gap or timeout sample is ended after this much
// silence, so the last frame of a burst is not held back.
constexpr auto kQuietFlush = std::chrono::milliseconds(150);
constexpr std::size_t kMaxQueuedFrames = 16;

std::string error_text(int code)
{
    return cp0::ui::lower(std::generic_category().message(code));
}

std::string device_path(int index)
{
    return "/dev/lirc" + std::to_string(index);
}

struct Devices {
    int rx = -1;
    int tx = -1;
    int rx_index = -1;
    int tx_index = -1;
    uint32_t tx_features = 0;
    std::string rx_error;

    void close_all()
    {
        if (tx >= 0 && tx != rx) ::close(tx);
        if (rx >= 0) ::close(rx);
        rx = tx = rx_index = tx_index = -1;
    }

    // Claims the first node that receives raw samples and the first that
    // sends pulses. On this board they are separate nodes (gpio-ir-tx and
    // gpio_ir_recv); one node offering both is handled too.
    void scan()
    {
        int denied = -1, first_errno = 0, opened = 0;
        for (int i = 0; i < kDeviceCount && (rx < 0 || tx < 0); ++i) {
            if (i == rx_index || i == tx_index) continue;
            const std::string path = device_path(i);
            int fd = -1;
            for (int mode : {O_RDWR, O_RDONLY, O_WRONLY}) {
                fd = ::open(path.c_str(), mode | O_NONBLOCK | O_CLOEXEC);
                if (fd >= 0) break;
                if (errno == EACCES && denied < 0) denied = i;
                if (first_errno == 0) first_errno = errno;
                if (errno == ENOENT) break;
            }
            if (fd < 0) continue;
            ++opened;
            uint32_t features = 0;
            bool used = false;
            if (ioctl(fd, LIRC_GET_FEATURES, &features) == 0) {
                const int access = fcntl(fd, F_GETFL) & O_ACCMODE;
                if (rx < 0 && (features & LIRC_CAN_REC_MODE2) && access != O_WRONLY) {
                    uint32_t mode = LIRC_MODE_MODE2;
                    ioctl(fd, LIRC_SET_REC_MODE, &mode);
                    rx = fd;
                    rx_index = i;
                    used = true;
                }
                if (tx < 0 && (features & LIRC_CAN_SEND_PULSE) && access != O_RDONLY) {
                    tx = fd;
                    tx_index = i;
                    tx_features = features;
                    used = true;
                }
            }
            if (!used) ::close(fd);
        }
        if (rx >= 0)
            rx_error.clear();
        else if (denied >= 0)
            rx_error = device_path(denied) + ": " + error_text(EACCES);
        else if (opened > 0 || tx >= 0)
            rx_error = "no raw receiver on /dev/lirc*";
        else
            rx_error = device_path(0) + ": " + error_text(first_errno ? first_errno : ENOENT);
    }

    std::string send(const std::vector<uint32_t> &pulses, uint32_t carrier_hz)
    {
        if (tx < 0) return "no ir emitter";
        if (pulses.empty()) return "nothing to send";
        uint32_t mode = LIRC_MODE_PULSE;
        ioctl(tx, LIRC_SET_SEND_MODE, &mode);
        if (tx_features & LIRC_CAN_SET_SEND_CARRIER) ioctl(tx, LIRC_SET_SEND_CARRIER, &carrier_hz);
        // The payload is an odd count of 32-bit microsecond values starting
        // with a pulse. Unverified: linux/lirc.h defines LIRC_MODE_PULSE but
        // does not describe the write format, and it has not been tried on
        // the device. The write is made blocking so it returns after the
        // frame is out.
        const int flags = fcntl(tx, F_GETFL);
        fcntl(tx, F_SETFL, flags & ~O_NONBLOCK);
        const std::size_t bytes = pulses.size() * sizeof(uint32_t);
        const ssize_t written = ::write(tx, pulses.data(), bytes);
        const int code = errno;
        fcntl(tx, F_SETFL, flags);
        if (written == static_cast<ssize_t>(bytes)) return {};
        return written < 0 ? "send: " + error_text(code) : std::string("send: short write");
    }
};
#endif

} // namespace

UIIrPage::UIIrPage() : AppPage()
{
    set_page_title("IR");
    reclaim_top_bar();
    remote_number_ = config_remote();
    pixels_.assign(static_cast<std::size_t>(kBurstW) * kBurstH, app_support::rgb565(cp0::theme::kPanel));
    load_remote();
    create_ui();
    draw_burst();
    render();
    worker_ = std::thread([this] { worker_loop(); });
    timer_ = lv_timer_create(timer_cb, kRefreshMs, this);
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

UIIrPage::~UIIrPage()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
    worker_stop_ = true;
    if (worker_.joinable()) worker_.join();
}

void UIIrPage::create_ui()
{
    if (!ui_APP_Container) return;
    cp0::ui::paint_background(ui_APP_Container);
    const cp0::ui::TitleRow row = cp0::ui::title_row(ui_APP_Container, "ir");
    status_ = row.status;

    const cp0::ui::Panel top = cp0::ui::panel(ui_APP_Container, 4, 19, 312, 36);
    if (top.box) {
        info_ = cp0::ui::label(top.box, 5, 3, cp0::theme::kDim);
        if (info_) {
            lv_obj_set_width(info_, 302);
            lv_label_set_long_mode(info_, LV_LABEL_LONG_CLIP);
        }
    }
    canvas_ = lv_canvas_create(ui_APP_Container);
    if (canvas_) {
        lv_canvas_set_buffer(canvas_, pixels_.data(), kBurstW, kBurstH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(canvas_, 10, 38);
    }

    for (std::size_t i = 0; i < ir::kSlotCount; ++i) {
        const int x = 4 + static_cast<int>(i % kColumns) * 79;
        const int y = 59 + static_cast<int>(i / kColumns) * 47;
        Tile &tile = tiles_[i];
        tile.panel = cp0::ui::panel(ui_APP_Container, x, y, 75, 44);
        if (!tile.panel.box) continue;
        tile.name = cp0::ui::label(tile.panel.box, 6, 5, cp0::theme::kText, true);
        if (tile.name) lv_label_set_text(tile.name, ir::kSlotNames[i]);
        tile.code = cp0::ui::label(tile.panel.box, 6, 29, cp0::theme::kDim);
    }
    hint_ = cp0::ui::footer(ui_APP_Container);
}

void UIIrPage::show_message(const std::string &text)
{
    message_ = text;
    message_until_ = lv_tick_get() + kMessageMs;
    render();
}

void UIIrPage::load_remote()
{
    ir::parse_remote(app_support::read_file(ir::remote_path(app_support::home_dir(), remote_number_)), remote_);
}

bool UIIrPage::save_remote()
{
    const std::string directory = app_support::home_dir() + "/ir";
    if (mkdir(directory.c_str(), 0755) != 0 && errno != EEXIST) return false;
    std::FILE *file = std::fopen(ir::remote_path(app_support::home_dir(), remote_number_).c_str(), "w");
    if (!file) return false;
    const std::string text = ir::serialize_remote(remote_);
    const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    return std::fclose(file) == 0 && written;
}

void UIIrPage::learn(const ir::Durations &frame, const ir::Code &code)
{
    ir::Button &button = remote_.buttons[static_cast<std::size_t>(learn_slot_)];
    button.code = code;
    button.raw = frame;
    // gpio_ir_recv delivers a demodulated signal, so the carrier is assumed
    // unless a device reports a measured one.
    button.carrier_hz = ir::kDefaultCarrierHz;
    const std::string name = ir::kSlotNames[learn_slot_];
    learn_slot_ = -1;
    show_message(save_remote() ? name + " learned" : "cannot write ~/ir");
}

void UIIrPage::send_selected()
{
    const ir::Button &button = remote_.buttons[static_cast<std::size_t>(selected_)];
    if (!button.learned()) {
        show_message("nothing learned");
        return;
    }
    std::vector<uint32_t> pulses = ir::tx_buffer(ir::button_durations(button));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tx_pending_ = std::move(pulses);
        tx_carrier_ = button.carrier_hz;
    }
    show_message(std::string("send ") + ir::kSlotNames[selected_]);
}

void UIIrPage::worker_loop()
{
#if CP0_IR_HAS_LIRC
    using Clock = std::chrono::steady_clock;
    Devices devices;
    ir::FrameAssembler assembler;
    Clock::time_point last_scan{};
    Clock::time_point last_data = Clock::now();
    bool scanned = false;
    uint32_t samples[256];

    auto publish = [&] {
        std::lock_guard<std::mutex> lock(mutex_);
        shared_rx_open_ = devices.rx >= 0;
        shared_rx_error_ = devices.rx_error;
        shared_scanned_ = true;
    };
    auto push = [&](ir::Durations &frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frames_.size() >= kMaxQueuedFrames) frames_.pop_front();
        frames_.push_back(std::move(frame));
        frame.clear();
    };

    while (!worker_stop_) {
        const Clock::time_point now = Clock::now();
        if ((devices.rx < 0 || devices.tx < 0) && (!scanned || now - last_scan >= kRescanEvery)) {
            devices.scan();
            scanned = true;
            last_scan = now;
            publish();
        }

        std::vector<uint32_t> pulses;
        uint32_t carrier = ir::kDefaultCarrierHz;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pulses.swap(tx_pending_);
            carrier = tx_carrier_;
        }
        if (!pulses.empty()) {
            const std::string result = devices.send(pulses, carrier);
            std::lock_guard<std::mutex> lock(mutex_);
            tx_result_ = result.empty() ? "sent" : result;
        }

        if (devices.rx < 0) {
            poll(nullptr, 0, kPollMs);
            continue;
        }
        pollfd entry{devices.rx, POLLIN, 0};
        const int ready = poll(&entry, 1, kPollMs);
        ir::Durations frame;
        if (ready > 0 && (entry.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            devices.close_all();
            devices.rx_error = "receiver closed";
            publish();
            continue;
        }
        if (ready > 0 && (entry.revents & POLLIN)) {
            const ssize_t count = read(devices.rx, samples, sizeof(samples));
            for (ssize_t i = 0; i < count / static_cast<ssize_t>(sizeof(samples[0])); ++i)
                if (assembler.feed(samples[i], frame)) push(frame);
            if (count > 0) last_data = Clock::now();
        } else if (assembler.pending() && Clock::now() - last_data >= kQuietFlush) {
            if (assembler.flush(frame)) push(frame);
        }
    }
    devices.close_all();
#else
    std::lock_guard<std::mutex> lock(mutex_);
    shared_rx_error_ = "lirc is not available in this build";
    shared_scanned_ = true;
#endif
}

void UIIrPage::draw_burst()
{
    if (pixels_.empty()) return;
    const uint16_t bg = app_support::rgb565(cp0::theme::kPanel);
    const uint16_t rule = app_support::rgb565(cp0::theme::kLine);
    const uint16_t on = app_support::rgb565(cp0::theme::kAccent);
    const std::vector<uint8_t> columns = ir::burst_columns(last_frame_, kBurstW);
    for (int y = 0; y < kBurstH; ++y)
        for (int x = 0; x < kBurstW; ++x) {
            uint16_t color = bg;
            if (y < kBurstH - 1 && columns[static_cast<std::size_t>(x)])
                color = on;
            else if (y == kBurstH - 2 && x % 2 == 0)
                color = rule;
            pixels_[static_cast<std::size_t>(y) * kBurstW + x] = color;
        }
    if (canvas_) lv_obj_invalidate(canvas_);
}

void UIIrPage::render()
{
    const bool learning = learn_slot_ >= 0;
    const ir::Button &current = remote_.buttons[static_cast<std::size_t>(selected_)];

    if (status_) {
        std::string text = "remote " + std::to_string(remote_number_);
        uint32_t color = cp0::theme::kIdle;
        if (lv_tick_get() < message_until_) {
            text = message_;
            color = cp0::theme::kText;
        } else if (learning) {
            text = std::string("learn ") + ir::kSlotNames[learn_slot_];
            color = cp0::theme::kWarn;
        } else if (scanned_ && !rx_open_) {
            text = "no receiver";
            color = cp0::theme::kDanger;
        } else if (current.learned()) {
            text += "  " + current.code.protocol + " " + ir::carrier_label(current.carrier_hz);
            color = cp0::theme::kAccent;
        }
        cp0::ui::set_text(status_, text.c_str(), color);
    }

    if (info_) {
        std::string text = "opening receiver";
        uint32_t color = cp0::theme::kDim;
        if (scanned_ && !rx_open_) {
            text = rx_error_;
            color = cp0::theme::kDanger;
        } else if (learning) {
            text = std::string("press the remote button for ") + ir::kSlotNames[learn_slot_];
            color = cp0::theme::kWarn;
        } else if (last_code_.valid()) {
            text = "last  " + ir::describe(last_code_) + "  repeat " + std::to_string(repeat_count_);
        } else if (scanned_) {
            text = "waiting for a remote";
        }
        cp0::ui::set_text(info_, text.c_str(), color);
    }

    for (std::size_t i = 0; i < ir::kSlotCount; ++i) {
        const Tile &tile = tiles_[i];
        const ir::Button &button = remote_.buttons[i];
        const bool focus = static_cast<int>(i) == selected_;
        const bool armed = static_cast<int>(i) == learn_slot_;
        cp0::ui::set_panel_focus(tile.panel, focus, armed ? cp0::theme::kWarn : cp0::theme::kAccent);
        cp0::ui::set_text_color(tile.name, armed   ? cp0::theme::kWarn
                                           : focus ? cp0::theme::kAccent
                                           : button.learned() ? cp0::theme::kText
                                                              : cp0::theme::kDim);
        cp0::ui::set_text(tile.code, button.learned() ? ir::code_label(button.code).c_str() : "--",
                          cp0::theme::kDim);
    }

    if (learning)
        cp0::ui::set_footer(hint_, {{"l", "cancel"}, {"esc", "cancel"}});
    else
        cp0::ui::set_footer(hint_, {{"l", "learn"}, {"ent", "send"}, {"del", "clear"}, {"tab", "remote"}});
}

void UIIrPage::timer_cb(lv_timer_t *timer) noexcept
{
    auto *page = static_cast<UIIrPage *>(lv_timer_get_user_data(timer));
    if (!page) return;
    try {
        page->on_timer();
    } catch (...) {
    }
}

void UIIrPage::on_timer()
{
    std::deque<ir::Durations> frames;
    std::string tx_result;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frames.swap(frames_);
        tx_result.swap(tx_result_);
        if (scanned_ != shared_scanned_ || rx_open_ != shared_rx_open_ || rx_error_ != shared_rx_error_) {
            scanned_ = shared_scanned_;
            rx_open_ = shared_rx_open_;
            rx_error_ = shared_rx_error_;
            changed = true;
        }
    }

    for (const ir::Durations &frame : frames) {
        const ir::Code code = ir::decode(frame);
        if (!code.valid()) continue;
        changed = true;
        if (code.repeat) {
            if (last_code_.valid()) ++repeat_count_;
            continue;
        }
        last_frame_ = frame;
        last_code_ = code;
        repeat_count_ = 0;
        draw_burst();
        if (learn_slot_ >= 0) learn(frame, code);
    }

    if (!tx_result.empty()) {
        show_message(tx_result);
        changed = false;
    }
    if (message_until_ != 0 && lv_tick_get() >= message_until_) {
        message_until_ = 0;
        changed = true;
    }
    if (changed) render();
}

void UIIrPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UIIrPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item) return;
    const uint32_t key = item->semantic_key ? item->semantic_key : item->key_code;
    try {
        page->handle_key(key, item->key_state == KBD_KEY_REPEATED);
    } catch (...) {
    }
}

void UIIrPage::handle_key(uint32_t key, bool repeat)
{
    const bool arrow = key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN;
    if (repeat && !arrow) return;
    if (key == KEY_ESC) {
        if (learn_slot_ >= 0) {
            learn_slot_ = -1;
            render();
        } else if (navigate_home) {
            navigate_home();
        }
        return;
    }
    if (key == KEY_L) {
        if (learn_slot_ >= 0) {
            learn_slot_ = -1;
        } else if (!rx_open_) {
            show_message("no receiver");
            return;
        } else {
            learn_slot_ = selected_;
        }
        render();
        return;
    }
    if (arrow) {
        const int count = static_cast<int>(ir::kSlotCount);
        if (key == KEY_LEFT) selected_ = (selected_ + count - 1) % count;
        if (key == KEY_RIGHT) selected_ = (selected_ + 1) % count;
        if (key == KEY_UP || key == KEY_DOWN) selected_ = (selected_ + kColumns) % count;
        if (learn_slot_ >= 0) learn_slot_ = selected_;
        render();
        return;
    }
    if (key == KEY_TAB) {
        learn_slot_ = -1;
        remote_number_ = remote_number_ % ir::kRemoteCount + 1;
        set_config_remote(remote_number_);
        load_remote();
        render();
        return;
    }
    if (key == KEY_ENTER || key == KEY_KPENTER) {
        if (learn_slot_ < 0) send_selected();
        return;
    }
    if (key == KEY_BACKSPACE || key == KEY_DELETE) {
        ir::Button &button = remote_.buttons[static_cast<std::size_t>(selected_)];
        if (!button.learned()) return;
        button = ir::Button{};
        show_message(save_remote() ? std::string(ir::kSlotNames[selected_]) + " cleared" : "cannot write ~/ir");
        return;
    }
}

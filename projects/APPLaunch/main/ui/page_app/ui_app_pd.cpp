/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_pd.hpp"

#include "../app_support.hpp"
#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "input_keys.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace theme = cp0::theme;

constexpr uint32_t kRefreshMs = 33;
constexpr uint32_t kStartTimeoutMs = 3000;
constexpr float kBarFloorDb = -80.0f;
constexpr float kBarDecayDb = 2.0f;
constexpr float kClipDb = -1.0f;

void set_visible(lv_obj_t *obj, bool on)
{
    if (!obj) return;
    if (on)
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

int bar_height(float db, int height)
{
    const float t = std::clamp((db - kBarFloorDb) / -kBarFloorDb, 0.0f, 1.0f);
    return static_cast<int>(std::lround(t * static_cast<float>(height)));
}

} // namespace

UIPdPage::UIPdPage() : AppPage()
{
    set_page_title("Pd");
    reclaim_top_bar();
    dir_ = app_support::home_dir() + "/pd";
    mkdir(dir_.c_str(), 0755);
    tools_ok_ = app_support::tool_present("pd") && app_support::tool_present("pw-jack");
    bars_.fill(kBarFloorDb);
    pixels_.assign(static_cast<std::size_t>(kCanvasW) * kCanvasH, app_support::rgb565(theme::kPanel));
    create_ui();
    scan();
    if (!tools_ok_) {
        console_.add_line("pd is not installed.");
        console_.add_line("apt install puredata-core");
        console_.add_line("  pipewire-jack");
    } else if (patches_.empty()) {
        console_.add_line("put .pd patches in ~/pd");
    }
    draw();
    render();
    timer_ = lv_timer_create(timer_cb, kRefreshMs, this);
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

UIPdPage::~UIPdPage()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
    try {
        stop(nullptr);
    } catch (...) {
    }
}

void UIPdPage::create_ui()
{
    if (!ui_APP_Container) return;
    cp0::ui::paint_background(ui_APP_Container);
    const cp0::ui::TitleRow title = cp0::ui::title_row(ui_APP_Container, "pd");
    status_ = title.status;

    cp0::ui::panel(ui_APP_Container, 4, 19, 118, 132);
    for (int i = 0; i < kRows; ++i) {
        row_boxes_[i] = cp0::ui::box(ui_APP_Container, 5, 22 + i * 14, 116, 12, theme::kSelect);
        if (!row_boxes_[i]) continue;
        row_labels_[i] = cp0::ui::label(row_boxes_[i], 3, 1, theme::kText);
        if (row_labels_[i]) {
            lv_obj_set_width(row_labels_[i], 112);
            lv_label_set_long_mode(row_labels_[i], LV_LABEL_LONG_CLIP);
        }
    }
    empty_ = cp0::ui::label(ui_APP_Container, 9, 23, theme::kIdle);

    cp0::ui::panel(ui_APP_Container, 126, 19, 190, 60);
    canvas_ = lv_canvas_create(ui_APP_Container);
    if (canvas_) {
        lv_canvas_set_buffer(canvas_, pixels_.data(), kCanvasW, kCanvasH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(canvas_, kCanvasX, kCanvasY);
    }

    cp0::ui::panel(ui_APP_Container, 126, 82, 190, 69);
    for (int i = 0; i < kConsoleRows; ++i) {
        console_labels_[i] = cp0::ui::label(ui_APP_Container, 130, 84 + i * 13, theme::kIdle);
        if (!console_labels_[i]) continue;
        lv_obj_set_width(console_labels_[i], 184);
        lv_label_set_long_mode(console_labels_[i], LV_LABEL_LONG_CLIP);
    }

    footer_ = cp0::ui::footer(ui_APP_Container);
}

void UIPdPage::scan()
{
    const std::string running = running_ >= 0 ? patches_[static_cast<std::size_t>(running_)] : std::string();
    const std::string selected =
        selection_ < static_cast<int>(patches_.size()) ? patches_[static_cast<std::size_t>(selection_)] : std::string();

    std::vector<std::string> names;
    if (DIR *dir = opendir(dir_.c_str())) {
        while (const dirent *entry = readdir(dir)) names.emplace_back(entry->d_name);
        closedir(dir);
    }
    patches_ = pd::patches_in(std::move(names));

    const auto index_of = [&](const std::string &name) {
        const auto found = std::find(patches_.begin(), patches_.end(), name);
        return found == patches_.end() ? -1 : static_cast<int>(found - patches_.begin());
    };
    selection_ = std::max(0, index_of(selected));
    running_ = running.empty() ? -1 : index_of(running);
}

bool UIPdPage::open_fifo(const std::string &path, int &fd)
{
    unlink(path.c_str());
    if (mkfifo(path.c_str(), 0600) != 0) return false;
    // The read end is opened first and non-blocking, so the shell's open for
    // writing does not wait and the UI thread never blocks on a read.
    fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) return true;
    unlink(path.c_str());
    return false;
}

void UIPdPage::close_fifo(std::string &path, int &fd)
{
    if (fd >= 0) close(fd);
    fd = -1;
    if (!path.empty()) unlink(path.c_str());
    path.clear();
}

void UIPdPage::start(int index)
{
    if (!tools_ok_ || index < 0 || index >= static_cast<int>(patches_.size())) return;
    stop(nullptr);
    console_.clear();

    const std::string stem = app_support::runtime_dir() + "/cp0-pd-" + std::to_string(getpid());
    out_path_ = stem + ".out";
    mon_path_ = stem + ".pcm";
    if (!open_fifo(out_path_, out_fd_)) {
        out_path_.clear();
        mon_path_.clear();
        console_.add_line("cannot create a pipe");
        return;
    }
    pd_pid_ = app_support::spawn(pd::launch_command(dir_ + "/" + patches_[static_cast<std::size_t>(index)], out_path_));
    if (pd_pid_ <= 0) {
        close_fifo(out_path_, out_fd_);
        mon_path_.clear();
        console_.add_line("cannot start pd");
        return;
    }
    if (open_fifo(mon_path_, mon_fd_))
        mon_pid_ = app_support::spawn("exec parec --device=@DEFAULT_MONITOR@ --format=s16le --rate=" +
                                      std::to_string(audio::kSampleRate) + " --channels=1 --raw --latency-msec=40 > " +
                                      pd::shell_quote(mon_path_) + " 2>/dev/null");
    else
        mon_path_.clear();

    running_ = index;
    state_ = State::Starting;
    started_ms_ = lv_tick_get();
}

void UIPdPage::stop(const char *note)
{
    app_support::stop(pd_pid_);
    app_support::stop(mon_pid_);
    close_fifo(out_path_, out_fd_);
    close_fifo(mon_path_, mon_fd_);
    samples_.clear();
    odd_byte_valid_ = false;
    level_db_ = audio::kFloorDb;
    console_.flush();
    if (note && state_ != State::Idle) console_.add_line(note);
    state_ = State::Idle;
    running_ = -1;
}

void UIPdPage::read_output()
{
    if (out_fd_ < 0) return;
    char buffer[2048];
    while (true) {
        const ssize_t count = read(out_fd_, buffer, sizeof(buffer));
        if (count > 0) {
            console_.feed(buffer, static_cast<std::size_t>(count));
            state_ = State::Running;
            continue;
        }
        // No data with a writer attached is EAGAIN; a read of 0 means no
        // process holds the pipe, which after the start is the end of pd.
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            state_ = State::Running;
        } else if (state_ == State::Running) {
            stop("-- pd ended --");
        } else if (lv_tick_get() - started_ms_ > kStartTimeoutMs) {
            stop("-- pd did not start --");
        }
        return;
    }
}

void UIPdPage::read_monitor()
{
    if (mon_fd_ < 0) return;
    uint8_t buffer[16384];
    while (true) {
        const ssize_t count = read(mon_fd_, buffer, sizeof(buffer));
        if (count <= 0) break;
        std::size_t index = 0;
        if (odd_byte_valid_) {
            samples_.push_back(static_cast<int16_t>(odd_byte_ | (buffer[0] << 8)));
            odd_byte_valid_ = false;
            index = 1;
        }
        for (; index + 1 < static_cast<std::size_t>(count); index += 2)
            samples_.push_back(static_cast<int16_t>(buffer[index] | (buffer[index + 1] << 8)));
        if (index < static_cast<std::size_t>(count)) {
            odd_byte_ = buffer[index];
            odd_byte_valid_ = true;
        }
    }
    const std::size_t keep = static_cast<std::size_t>(audio::kFftSize);
    if (samples_.size() > keep) samples_.erase(samples_.begin(), samples_.end() - keep);
}

void UIPdPage::analyse()
{
    if (samples_.size() < static_cast<std::size_t>(audio::kFftSize)) {
        for (float &bar : bars_) bar = std::max(kBarFloorDb, bar - kBarDecayDb);
        return;
    }
    const std::vector<float> db = audio::spectrum_db(samples_.data(), samples_.size());
    level_db_ = audio::rms_dbfs(samples_.data(), samples_.size());
    const std::vector<float> bands = audio::log_bands(db, audio::kSampleRate, kBands, audio::kMinHz, audio::kMaxHz);
    for (int i = 0; i < kBands; ++i) bars_[i] = std::max(bands[i], bars_[i] - kBarDecayDb);
}

void UIPdPage::draw()
{
    if (!canvas_) return;
    const uint16_t bar = app_support::rgb565(theme::kAccent);
    const uint16_t clip = app_support::rgb565(theme::kDanger);
    const uint16_t base = app_support::rgb565(theme::kLine);
    std::fill(pixels_.begin(), pixels_.end(), app_support::rgb565(theme::kPanel));
    const auto put = [&](int x, int y, uint16_t color) {
        if (x >= 0 && x < kCanvasW && y >= 0 && y < kCanvasH)
            pixels_[static_cast<std::size_t>(y) * kCanvasW + x] = color;
    };
    for (int x = 0; x < kCanvasW; ++x) put(x, kCanvasH - 1, base);
    for (int band = 0; band < kBands; ++band) {
        const int x0 = band * kBandW;
        const int h = bar_height(bars_[band], kCanvasH - 1);
        const uint16_t color = bars_[band] >= kClipDb ? clip : bar;
        for (int y = kCanvasH - 1 - h; y < kCanvasH - 1; ++y) {
            if ((kCanvasH - 1 - y) % 3 == 0) continue;
            for (int x = x0; x < x0 + kBandW - 1; ++x) put(x, y, color);
        }
    }
    lv_obj_invalidate(canvas_);
}

void UIPdPage::render()
{
    const int count = static_cast<int>(patches_.size());
    set_visible(empty_, count == 0);
    if (count == 0 && empty_) cp0::ui::set_text(empty_, "no patches", theme::kIdle);
    int first = std::clamp(selection_ - kRows + 1, 0, std::max(0, count - kRows));
    if (selection_ < first) first = selection_;
    for (int i = 0; i < kRows; ++i) {
        const int index = first + i;
        const bool used = index < count;
        set_visible(row_boxes_[i], used);
        if (!used || !row_boxes_[i]) continue;
        const bool on = index == selection_;
        const lv_opa_t opa = on ? LV_OPA_COVER : LV_OPA_TRANSP;
        if (lv_obj_get_style_bg_opa(row_boxes_[i], LV_PART_MAIN) != opa) lv_obj_set_style_bg_opa(row_boxes_[i], opa, 0);
        const std::string text = std::string(index == running_ ? "~ " : on ? "> " : "  ") +
                                 pd::patch_label(patches_[static_cast<std::size_t>(index)]).substr(0, kNameColumns);
        cp0::ui::set_text(row_labels_[i], text.c_str(),
                          index == running_ ? theme::kAccent : on ? theme::kText : theme::kIdle);
    }

    const std::vector<std::string> lines = console_.tail(kConsoleRows);
    for (int i = 0; i < kConsoleRows; ++i) {
        const bool used = i < static_cast<int>(lines.size());
        const bool notice = used && lines[static_cast<std::size_t>(i)].rfind("--", 0) == 0;
        cp0::ui::set_text(console_labels_[i], used ? lines[static_cast<std::size_t>(i)].c_str() : "",
                          notice ? theme::kWarn : theme::kText);
    }

    if (!tools_ok_) {
        cp0::ui::set_text(status_, "no pd", theme::kDanger);
    } else if (state_ == State::Starting) {
        cp0::ui::set_text(status_, "starting", theme::kWarn);
    } else if (state_ == State::Running) {
        std::string text = pd::format_elapsed((lv_tick_get() - started_ms_) / 1000);
        if (samples_.size() >= static_cast<std::size_t>(audio::kFftSize))
            text += "  " + std::to_string(static_cast<int>(std::lround(level_db_))) + " dBFS";
        cp0::ui::set_text(status_, text.c_str(), theme::kAccent);
    } else {
        const std::string text = std::to_string(count) + (count == 1 ? " patch" : " patches");
        cp0::ui::set_text(status_, text.c_str(), theme::kIdle);
    }

    const bool on_running = state_ != State::Idle && selection_ == running_;
    if (state_ != State::Idle)
        cp0::ui::set_footer(footer_, {{"enter", on_running ? "stop" : "run"}, {"up/dn", "patch"}, {"s", "stop"}});
    else
        cp0::ui::set_footer(footer_, {{"enter", "run"}, {"up/dn", "patch"}, {"r", "rescan"}});
}

void UIPdPage::on_timer()
{
    if (state_ != State::Idle) {
        read_output();
        read_monitor();
    }
    analyse();
    draw();
    render();
}

void UIPdPage::timer_cb(lv_timer_t *timer) noexcept
{
    auto *page = static_cast<UIPdPage *>(lv_timer_get_user_data(timer));
    if (!page) return;
    try {
        page->on_timer();
    } catch (...) {
    }
}

void UIPdPage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UIPdPage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item) return;
    const bool repeat = item->key_state == KBD_KEY_REPEATED;
    const uint32_t key = item->semantic_key ? item->semantic_key : item->key_code;
    if (repeat && key != KEY_UP && key != KEY_DOWN) return;
    try {
        page->handle_key(key);
    } catch (...) {
    }
}

void UIPdPage::handle_key(uint32_t key)
{
    const int count = static_cast<int>(patches_.size());
    switch (key) {
    case KEY_ESC:
        stop(nullptr);
        if (navigate_home) navigate_home();
        return;
    case KEY_UP:
        if (count > 0) selection_ = (selection_ + count - 1) % count;
        break;
    case KEY_DOWN:
        if (count > 0) selection_ = (selection_ + 1) % count;
        break;
    case KEY_ENTER:
    case KEY_KPENTER:
        if (state_ != State::Idle && selection_ == running_)
            stop("-- stopped --");
        else
            start(selection_);
        break;
    case KEY_S:
        stop("-- stopped --");
        break;
    case KEY_R:
        scan();
        break;
    default:
        return;
    }
    render();
}

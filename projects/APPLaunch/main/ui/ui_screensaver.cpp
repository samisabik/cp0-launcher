/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "ui_screensaver.h"
#include "cp0_theme.hpp"
#include "ui_low_battery.h"

#include "cp0_lvgl_app.h"
#include "cp0_enum_cast.h"
#include "hal_lvgl_bsp.h"
#include "keyboard_input.h"
#include "launcher_media_controls.h"
#include "launcher_platform.hpp"
#include "launcher_toast.h"
#include "lvgl/lvgl.h"
#include "model/life_model.hpp"
#include "model/lockscreen_status_model.hpp"
#include "settings/settings_battery_info_model.hpp"
#include "settings/settings_prefs.hpp"
#include "model/lockscreen_state_model.hpp"
#include "model/screensaver_model.hpp"
#include "model/screensaver_runtime_contract.hpp"
#include "ui_app_page.hpp"
#include "cp0_font_service.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr uint32_t kIdleCheckMs = 500;
constexpr uint32_t kIdleMaxMs = 5000;
constexpr uint32_t kHoldPollMs = 100;
constexpr uint32_t kExitAnimationMs = 200;
/* Shown once the idle TAB hold matures past the model's hint delay. */
constexpr const char *kHoldHintText = "hold tab 3s to lock";

lv_obj_t *s_overlay = nullptr;
lv_obj_t *s_hint = nullptr;
lv_obj_t *s_content = nullptr;
lv_obj_t *s_clock = nullptr;
lv_obj_t *s_date = nullptr;
lv_obj_t *s_prompt_text = nullptr;
const uint32_t &kColorAccent = cp0::theme::kAccent;
const uint32_t &kColorText = cp0::theme::kText;
static const uint32_t &kColorDim = cp0::theme::kDim;
static const uint32_t &kColorIdle = cp0::theme::kIdle;
constexpr lv_coord_t kContentLeft = 16;
constexpr lv_coord_t kClockTop = 16;
constexpr lv_coord_t kDateLeft = 96;
constexpr lv_coord_t kDateTop = 24;
constexpr lv_coord_t kStatusTop = 48;
constexpr lv_coord_t kStatusRowH = 14;
constexpr lv_coord_t kPromptTop = 104;
constexpr uint32_t kStatusRefreshMs = 5000;
constexpr int kStatusRows = 4;
lv_obj_t *s_status_values[kStatusRows] = {};
uint32_t s_status_refreshed = 0;
lv_timer_t *s_timer = nullptr;

/* Game of Life behind the clock while the lock panel is visible. It pauses
 * whenever the panel is black, so a locked, dark device does no work. */
constexpr int kLifeCell = 4;
constexpr int kLifeCols = 80;
constexpr int kLifeRows = 42;
constexpr int kLifeCanvasTop = 1;
constexpr int kCardWidth = 190;
constexpr int kCardHeight = kPromptTop - kClockTop + 26;
constexpr uint32_t kLifeStepMs = 110;
constexpr uint64_t kLifeMaxGenerations = 900;
constexpr uint64_t kLifeGliderEvery = 90;
lv_obj_t *s_life_canvas = nullptr;
lv_timer_t *s_life_timer = nullptr;
std::vector<uint16_t> s_life_pixels;
// Colour last written for each cell; 1 never occurs as a cell colour, so it
// forces a full redraw after reseeding.
std::vector<uint16_t> s_life_drawn;
constexpr uint16_t kLifeUndrawn = 1;
LifeGrid s_life(kLifeCols, kLifeRows);
uint32_t s_life_rng = 0x2545F491u;
void life_run(bool running);
ScreensaverModel s_model;
LockscreenStateModel s_lock;
bool s_exiting = false;
/* Raw backlight value captured when the panel is forced dark, or -1 while the
 * backlight is under normal control. */
int s_screen_off_backlight_raw = -1;

struct ScreensaverPanel
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool black = false;
};

/* While the screensaver is up the panel is pure black over the whole display, so
 * it reads as a black screen on every backend - including the ones whose
 * backlight write is a stub that reports success without dimming anything.
 * Driving the backlight down is a power optimisation on top of that, never the
 * reason the screen looks black. */
ScreensaverPanel full_screen_panel()
{
    ScreensaverPanel panel;
    lv_display_t *display = lv_display_get_default();
    panel.width = display ? lv_display_get_horizontal_resolution(display) : 0;
    panel.height = display ? lv_display_get_vertical_resolution(display) : 0;
    panel.black = true;
    return panel;
}

/* The cached wallpaper fills the application area below the page's top bar. */
/* The visible states cover the top bar too: the card carries the time and
 * status, and the simulation gets the whole display. */
ScreensaverPanel wake_panel()
{
    ScreensaverPanel panel = full_screen_panel();
    panel.black = false;
    return panel;
}

/* Panel currently in effect.  Animation must use these numbers rather than read
 * the object's geometry: the model is activated with them, and an object that
 * has not been through a layout pass reports stale (zero) dimensions. */
ScreensaverPanel s_panel;

void apply_panel(const ScreensaverPanel &panel)
{
    if (!s_overlay) return;
    s_panel = panel;
    lv_obj_set_pos(s_overlay, panel.x, panel.y);
    lv_obj_set_size(s_overlay, panel.width, panel.height);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    if (s_content) {
        if (panel.black) lv_obj_add_flag(s_content, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_content, LV_OBJ_FLAG_HIDDEN);
    }
    life_run(!panel.black);
}

void refresh_clock()
{
    if (!s_clock || !s_date) return;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (!localtime_r(&now, &local)) return;
    const std::string clock = settings_prefs::format_clock(local);
    const char *clock_text = clock.c_str();
    char date_text[24];
    std::strftime(date_text, sizeof(date_text), "%a %d %b", &local);
    for (char *c = date_text; *c; ++c)
        *c = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
    if (std::strcmp(lv_label_get_text(s_clock), clock_text) != 0) lv_label_set_text(s_clock, clock_text);
    if (std::strcmp(lv_label_get_text(s_date), date_text) != 0) lv_label_set_text(s_date, date_text);
}

lv_obj_t *content_label(lv_obj_t *parent, uint16_t size, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    if (!label) return nullptr;
    lv_obj_set_style_text_font(label, cp0_fonts().get_chrome(size), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, "");
    return label;
}

std::string interface_ipv4(const char *name)
{
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) return {};
    std::string address;
    for (ifaddrs *entry = list; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET || std::strcmp(entry->ifa_name, name) != 0)
            continue;
        char text[INET_ADDRSTRLEN] = {};
        const auto *in = reinterpret_cast<const sockaddr_in *>(entry->ifa_addr);
        if (inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text))) address = text;
        break;
    }
    freeifaddrs(list);
    return address;
}

std::string battery_status()
{
    std::string payload;
    int code = -1;
    cp0_signal_bq27220_api({"Read"}, [&](int result, std::string data) {
        code = result;
        payload = std::move(data);
    });
    SettingsBatterySnapshot battery;
    if (code != 0 || !SettingsBatteryInfoModel::parse_payload(payload, battery)) return "--";
    return lockscreen_status::battery_line(battery.soc, battery.remain_mah, battery.avg_current_ma);
}

std::string network_status()
{
    cp0_wifi_status_t wifi{};
    if (cp0_wifi_status_read(&wifi) == 0 && wifi.connected && wifi.ssid[0]) return wifi.ssid;
    if (!interface_ipv4("eth0").empty()) return "ethernet";
    return "offline";
}

std::string uptime_status()
{
    std::FILE *file = std::fopen("/proc/uptime", "r");
    double seconds = 0.0;
    if (file) {
        if (std::fscanf(file, "%lf", &seconds) != 1) seconds = 0.0;
        std::fclose(file);
    }
    return lockscreen_status::uptime_line(static_cast<long>(seconds));
}

void refresh_status(bool force)
{
    const uint32_t now = lv_tick_get();
    if (!force && now - s_status_refreshed < kStatusRefreshMs) return;
    s_status_refreshed = now;
    const std::string tailscale = interface_ipv4("tailscale0");
    const std::string values[kStatusRows] = {
        battery_status(), network_status(), tailscale.empty() ? std::string("off") : tailscale, uptime_status()};
    for (int i = 0; i < kStatusRows; ++i)
        if (s_status_values[i] && values[i] != lv_label_get_text(s_status_values[i]))
            lv_label_set_text(s_status_values[i], values[i].c_str());
}

uint32_t life_random()
{
    s_life_rng ^= s_life_rng << 13;
    s_life_rng ^= s_life_rng >> 17;
    s_life_rng ^= s_life_rng << 5;
    return s_life_rng;
}

uint16_t rgb565(uint32_t rgb)
{
    return lv_color_to_u16(lv_color_hex(rgb));
}

/* Newborn cells flash launcher green, settle into a deep green, and leave a
 * short trail that fades to black after they die. */
uint16_t life_color(int8_t age)
{
    static uint16_t kAlive[6];
    static uint16_t kTrail[8];
    static uint32_t built_for = 0xFFFFFFFFu;
    if (built_for != cp0::theme::kAccent) {
        built_for = cp0::theme::kAccent;
        static const int kAliveScale[6] = {100, 87, 72, 59, 50, 44};
        static const int kTrailScale[8] = {28, 23, 18, 15, 11, 9, 6, 3};
        const auto scaled = [](int percent) {
            const uint32_t c = cp0::theme::kAccent;
            const uint32_t r = ((c >> 16) & 0xFF) * percent / 100, g = ((c >> 8) & 0xFF) * percent / 100,
                           bl = (c & 0xFF) * percent / 100;
            return rgb565((r << 16) | (g << 8) | bl);
        };
        for (int i = 0; i < 6; ++i) kAlive[i] = scaled(kAliveScale[i]);
        for (int i = 0; i < 8; ++i) kTrail[i] = scaled(kTrailScale[i]);
    }
    if (age > 0) return kAlive[std::min<int>(age, 6) - 1];
    const int faded = -age;
    return faded < 8 ? kTrail[faded] : 0;
}

void life_seed()
{
    s_life_drawn.clear();
    s_life_rng ^= lv_tick_get() * 2654435761u ^ static_cast<uint32_t>(std::time(nullptr));
    s_life.randomize(life_random(), 22 + static_cast<int>(life_random() % 10));
}

/* Cells completely hidden by the status card are never drawn. */
bool life_cell_covered(int x, int y)
{
    const int left = kContentLeft - 8;
    const int top = kClockTop - 6 - kLifeCanvasTop;
    const int right = left + kCardWidth;
    const int bottom = top + kCardHeight;
    return x * kLifeCell >= left && (x + 1) * kLifeCell <= right && y * kLifeCell >= top &&
           (y + 1) * kLifeCell <= bottom;
}

/* Writes only cells whose colour changed and invalidates the changed rows as
 * merged bands, so a quiet pattern costs a few small SPI transfers. */
void life_draw()
{
    if (!s_life_canvas || s_life_pixels.empty()) return;
    if (s_life_drawn.size() != static_cast<std::size_t>(kLifeCols * kLifeRows))
        s_life_drawn.assign(static_cast<std::size_t>(kLifeCols * kLifeRows), kLifeUndrawn);
    const int stride = kLifeCols * kLifeCell;
    lv_area_t canvas_area;
    lv_obj_get_coords(s_life_canvas, &canvas_area);

    int band_top = -1;
    int band_left = kLifeCols;
    int band_right = -1;
    int bands = 0;
    std::vector<lv_area_t> areas;
    auto close_band = [&](int end_row) {
        if (band_top < 0) return;
        lv_area_t area;
        area.x1 = canvas_area.x1 + band_left * kLifeCell;
        area.x2 = canvas_area.x1 + (band_right + 1) * kLifeCell - 1;
        area.y1 = canvas_area.y1 + band_top * kLifeCell;
        area.y2 = canvas_area.y1 + end_row * kLifeCell - 1;
        areas.push_back(area);
        ++bands;
        band_top = -1;
        band_left = kLifeCols;
        band_right = -1;
    };

    for (int y = 0; y < kLifeRows; ++y) {
        int row_left = kLifeCols;
        int row_right = -1;
        for (int x = 0; x < kLifeCols; ++x) {
            if (life_cell_covered(x, y)) continue;
            const uint16_t color = life_color(s_life.age(x, y));
            uint16_t &drawn = s_life_drawn[static_cast<std::size_t>(y) * kLifeCols + x];
            if (drawn == color) continue;
            drawn = color;
            uint16_t *row = &s_life_pixels[static_cast<std::size_t>(y * kLifeCell) * stride + x * kLifeCell];
            for (int dy = 0; dy < kLifeCell - 1; ++dy, row += stride)
                std::fill_n(row, kLifeCell - 1, color);
            row_left = std::min(row_left, x);
            row_right = std::max(row_right, x);
        }
        if (row_right < 0) {
            close_band(y);
            continue;
        }
        if (band_top < 0) band_top = y;
        band_left = std::min(band_left, row_left);
        band_right = std::max(band_right, row_right);
    }
    close_band(kLifeRows);

    // LVGL keeps a short list of dirty areas; past that a full redraw is cheaper.
    if (bands > 12) {
        lv_obj_invalidate(s_life_canvas);
        return;
    }
    for (const lv_area_t &area : areas) lv_obj_invalidate_area(s_life_canvas, &area);
}

void life_timer_cb(lv_timer_t *) noexcept
{
    try {
        s_life.step();
        if (s_life.stagnant() || s_life.population() < 12 || s_life.generation() > kLifeMaxGenerations) {
            life_seed();
        } else if (s_life.generation() % kLifeGliderEvery == 0) {
            s_life.glider(static_cast<int>(life_random() % kLifeCols), static_cast<int>(life_random() % kLifeRows));
        }
        life_draw();
    } catch (...) {
    }
}

void life_run(bool running)
{
    if (!s_life_canvas) return;
    if (running) {
        if (!s_life_timer) {
            life_seed();
            life_draw();
            s_life_timer = lv_timer_create(life_timer_cb, kLifeStepMs, nullptr);
        }
    } else if (s_life_timer) {
        lv_timer_delete(s_life_timer);
        s_life_timer = nullptr;
    }
}

/* Clock, date and prompt in the launcher's terminal style. They are children of
 * the overlay so they slide away with it on unlock. */
void create_content()
{
    if (!s_overlay || s_content) return;
    s_content = lv_obj_create(s_overlay);
    if (!s_content) return;
    lv_obj_remove_style_all(s_content);
    lv_obj_set_size(s_content, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s_content, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    s_life_pixels.assign(static_cast<std::size_t>(kLifeCols * kLifeCell) * kLifeRows * kLifeCell, 0);
    s_life_canvas = lv_canvas_create(s_content);
    if (s_life_canvas) {
        lv_canvas_set_buffer(s_life_canvas, s_life_pixels.data(), kLifeCols * kLifeCell, kLifeRows * kLifeCell,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(s_life_canvas, 0, kLifeCanvasTop);
    }

    // A dark panel keeps the clock readable over the simulation.
    if (lv_obj_t *card = lv_obj_create(s_content)) {
        lv_obj_remove_style_all(card);
        lv_obj_set_pos(card, kContentLeft - 8, kClockTop - 6);
        lv_obj_set_size(card, kCardWidth, kCardHeight);
        lv_obj_set_style_bg_color(card, lv_color_hex(cp0::theme::kPanel), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_clear_flag(card, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        cp0::ui::brackets(card, cp0::theme::kAccent, 5);
    }

    s_clock = content_label(s_content, kCp0ChromeFontSize * 2, kColorText);
    if (s_clock) lv_obj_set_pos(s_clock, kContentLeft, kClockTop);
    s_date = content_label(s_content, kCp0ChromeFontSize, kColorDim);
    if (s_date) lv_obj_set_pos(s_date, kDateLeft, kDateTop);

    static const char *const kStatusKeys[kStatusRows] = {"bat", "net", "ts", "up"};
    for (int i = 0; i < kStatusRows; ++i) {
        const lv_coord_t y = kStatusTop + i * kStatusRowH;
        if (lv_obj_t *key = content_label(s_content, kCp0ChromeFontSize, kColorDim)) {
            lv_label_set_text(key, kStatusKeys[i]);
            lv_obj_set_pos(key, kContentLeft, y);
        }
        s_status_values[i] = content_label(s_content, kCp0ChromeFontSize, kColorIdle);
        if (s_status_values[i]) {
            lv_obj_set_pos(s_status_values[i], kContentLeft + 34, y);
            lv_obj_set_width(s_status_values[i], 140);
            lv_label_set_long_mode(s_status_values[i], LV_LABEL_LONG_CLIP);
        }
    }

    lv_obj_t *prompt = lv_obj_create(s_content);
    if (!prompt) return;
    lv_obj_remove_style_all(prompt);
    lv_obj_set_size(prompt, LV_SIZE_CONTENT, 26);
    lv_obj_set_pos(prompt, kContentLeft, kPromptTop);
    lv_obj_set_flex_flow(prompt, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(prompt, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (lv_obj_t *dollar = content_label(prompt, kCp0ChromeFontSize, kColorDim))
        lv_label_set_text(dollar, "$ ");
    s_prompt_text = content_label(prompt, kCp0ChromeFontSize, kColorText);
}

/* The title mask is a top-layer sibling: the wallpaper's application-area
 * bounds must not clip text in the status bar above it. */
void hide_hint()
{
    if (s_hint) lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    if (s_prompt_text) lv_label_set_text(s_prompt_text, "");
}

void show_hint(const char *text)
{
    if (!s_prompt_text || !text) return;
    lv_label_set_text(s_prompt_text, text);
    refresh_clock();
}

/* The launcher toast is shared with the other global hints, so only clear it
 * while this gesture owns it.  The following activate()/deactivate() resets the
 * model flag; hiding here is what stops the persistent hint outliving the hold. */
void hide_hold_hint()
{
    if (!s_model.hold_hint_visible()) return;
    launcher_toast().hide();
}

void update_timer_period()
{
    if (!s_timer) return;
    /* The panel is static in every state, so no animation frames are needed;
     * only a maturing hold is polled faster than the idle check. */
    lv_timer_set_period(s_timer, s_model.hold_pending() ? kHoldPollMs : kIdleCheckMs);
}

void suspend_screen_off_backlight() noexcept
{
    if (s_screen_off_backlight_raw >= 0) return;
    try {
        s_screen_off_backlight_raw = launcher_media_controls::suspend_backlight();
    } catch (...) {
        s_screen_off_backlight_raw = -1;
    }
}

void release_screen_off_backlight() noexcept
{
    if (s_screen_off_backlight_raw < 0) return;
    const int raw = s_screen_off_backlight_raw;
    /* Clear first so a throwing backend cannot latch the panel dark. */
    s_screen_off_backlight_raw = -1;
    try {
        launcher_media_controls::restore_backlight(raw);
    } catch (...) {
    }
}

void curtain_exit_anim_exec(void *object, int32_t y) noexcept
{
    try {
        if (s_exiting && object && object == s_overlay)
            lv_obj_set_y(static_cast<lv_obj_t *>(object), y);
    } catch (...) {
    }
}

void finish_screensaver_exit() noexcept
{
    release_screen_off_backlight();
    try {
        life_run(false);
        if (s_overlay) {
            lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_y(s_overlay, 0);
        }
        if (lv_obj_t *active_screen = lv_screen_active())
            lv_obj_invalidate(active_screen);
        // Start a full idle interval once the working screen is usable again.
        s_model.reset(lv_tick_get());
        update_timer_period();
    } catch (...) {
    }
    s_exiting = false;
    launcher_battery_ui::refresh_visibility();
}

void curtain_exit_anim_completed(lv_anim_t *animation) noexcept
{
    try {
        if (!animation || lv_anim_get_user_data(animation) != s_overlay || !s_exiting)
            return;
        finish_screensaver_exit();
    } catch (...) {
        finish_screensaver_exit();
    }
}

void cancel_exit_animation() noexcept
{
    if (s_overlay)
        lv_anim_delete(s_overlay, curtain_exit_anim_exec);
    s_exiting = false;
}

void overlay_delete_cb(lv_event_t *event) noexcept
{
    try {
        if (!event || !screensaver_delete_is_tracked(
                lv_event_get_target(event), lv_event_get_current_target(event), s_overlay))
            return;
        s_overlay = nullptr;
        s_content = s_clock = s_date = s_prompt_text = nullptr;
        for (lv_obj_t *&value : s_status_values) value = nullptr;
        s_life_canvas = nullptr;
        if (s_life_timer) lv_timer_delete(s_life_timer);
        s_life_timer = nullptr;
        if (s_hint) lv_obj_delete(s_hint);
        s_hint = nullptr;
        s_exiting = false;
        s_model.deactivate();
        release_screen_off_backlight();
        update_timer_period();
    } catch (...) {
        s_overlay = nullptr;
        s_content = s_clock = s_date = s_prompt_text = nullptr;
        for (lv_obj_t *&value : s_status_values) value = nullptr;
        s_life_canvas = nullptr;
        if (s_life_timer) lv_timer_delete(s_life_timer);
        s_life_timer = nullptr;
        if (s_hint) lv_obj_delete(s_hint);
        s_hint = nullptr;
        s_exiting = false;
        release_screen_off_backlight();
        update_timer_period();
        try {
            s_model.deactivate();
        } catch (...) {
        }
    }
}

void hint_delete_cb(lv_event_t *event) noexcept
{
    try {
        if (!event || !screensaver_delete_is_tracked(
                lv_event_get_target(event), lv_event_get_current_target(event), s_hint))
            return;
        s_hint = nullptr;
    } catch (...) {
        s_hint = nullptr;
    }
}

int timeout_seconds() noexcept
{
    try {
    bool succeeded = false;
    std::string response;
    cp0_signal_config_api({"GetInt", "dark_time", "30"},
                          [&](int code, std::string data) {
                              succeeded = code == 0;
                              response = std::move(data);
                          });
    return screensaver_timeout_from_config(succeeded, response);
    } catch (...) {
        return screensaver_timeout_from_config(false, {});
    }
}

void create_objects()
{
    lv_display_t *display = lv_display_get_default();
    if (!display)
        return;

    if (!s_overlay) {
        lv_obj_t *parent = lv_layer_top();
        if (!parent)
            return;
        s_overlay = lv_obj_create(parent);
        if (!s_overlay)
            return;
        lv_obj_add_event_cb(s_overlay, overlay_delete_cb, LV_EVENT_DELETE, nullptr);
        lv_obj_remove_style_all(s_overlay);
        apply_panel(full_screen_panel());
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
        create_content();
    }

    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

void stop_screensaver(bool was_active = false, bool animated = false)
{
    const bool active = was_active || s_model.active();

    /* Bring the panel back before the exit animation so wake-up is visible, and
     * drop the lock so the next entry starts black. */
    release_screen_off_backlight();
    hide_hint();
    hide_hold_hint();
    s_lock.reset(lv_tick_get());
    s_model.deactivate();
    update_timer_period();

    if (active && animated && s_overlay) {
        /* Slide the lock panel away; the simulation freezes for the slide. */
        apply_panel(wake_panel());
        life_run(false);
        if (s_panel.height > 0) {
            cancel_exit_animation();
            s_exiting = true;
            lv_anim_t animation;
            lv_anim_init(&animation);
            lv_anim_set_var(&animation, s_overlay);
            lv_anim_set_values(&animation, s_panel.y, -s_panel.height);
            lv_anim_set_duration(&animation, kExitAnimationMs);
            lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
            lv_anim_set_exec_cb(&animation, curtain_exit_anim_exec);
            lv_anim_set_user_data(&animation, s_overlay);
            lv_anim_set_completed_cb(&animation, curtain_exit_anim_completed);
            if (lv_anim_start(&animation))
                return;
            s_exiting = false;
        }
    }

    cancel_exit_animation();
    finish_screensaver_exit();
}

/* (1) locked: pure black over the whole display.  Both entry points (the idle
 * timeout and the long-press gesture) start here, so they behave identically.
 * The backlight goes down as a power optimisation, never as the reason the
 * screen looks black. */
void enter_lockscreen()
{
    create_objects();
    if (!s_overlay)
        return;

    const ScreensaverPanel panel = full_screen_panel();
    if (panel.width <= 0 || panel.height <= 0) return;

    cancel_exit_animation();
    apply_panel(panel);
    hide_hint();
    hide_hold_hint();

    const uint32_t now = lv_tick_get();
    s_model.activate();
    s_lock.reset(now);
    launcher_battery_ui::refresh_visibility();

    suspend_screen_off_backlight();

    lv_obj_move_foreground(s_overlay);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    update_timer_period();
    if (s_timer) lv_timer_reset(s_timer);
}

/* (2)/(3): restore the backlight and replace the top-bar title with the hint. */
void show_lockscreen_panel()
{
    apply_panel(wake_panel());
    lv_obj_move_foreground(s_overlay);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    release_screen_off_backlight();
    show_hint(s_lock.state() == LockscreenState::Armed
                  ? "press enter to unlock"
                  : "press tab to unlock");
    refresh_status(true);
}

/* Back to (1) after the visible state timed out. */
void sleep_lockscreen()
{
    apply_panel(full_screen_panel());
    hide_hint();
    suspend_screen_off_backlight();
}

void timer_cb(lv_timer_t *timer) noexcept
{
    try {
    if (!screensaver_timer_is_current(timer, s_timer)) return;
    if (!s_model.foreground() || LVGL_RUN_FLAGE != 1 || s_exiting)
        return;

    const uint32_t now = lv_tick_get();
    if (s_model.active()) {
        /* The visible lock states fall back to the black screen when idle. */
        if (s_lock.poll(now).sleep)
            sleep_lockscreen();
        else if (!s_panel.black) {
            refresh_clock();
            refresh_status(false);
        }
        return;
    }

    const ScreensaverHoldDecision hold = s_model.poll_hold(now);
    if (hold.show_hint)
        launcher_toast().show_persistent(kHoldHintText);
    if (hold.fire) {
        enter_lockscreen();
        return;
    }

    const uint32_t timeout_ms = static_cast<uint32_t>(timeout_seconds()) * 1000u;
    if (s_model.should_activate(now, timeout_ms, true)) {
        enter_lockscreen();
        return;
    }
    if (!s_model.hold_pending()) {
        // Sleep until the idle timeout is due; key activity only pushes it later.
        const uint32_t elapsed = now - s_model.last_activity_tick();
        const uint32_t remaining =
            timeout_ms > elapsed ? timeout_ms - elapsed : kIdleCheckMs;
        lv_timer_set_period(s_timer, std::clamp(remaining, kIdleCheckMs, kIdleMaxMs));
    }
    } catch (...) {
        stop_screensaver();
    }
}

} // namespace

extern "C" void ui_screensaver_init(void)
{
    try {
    if (s_timer)
        return;
    create_objects();
    s_model.set_foreground(true, lv_tick_get());
    s_timer = lv_timer_create(timer_cb, kIdleCheckMs, nullptr);
    } catch (...) {
        if (s_timer) {
            lv_timer_delete(s_timer);
            s_timer = nullptr;
        }
        s_model.set_foreground(false, 0);
    }
}

extern "C" void ui_screensaver_deinit(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    cancel_exit_animation();
    release_screen_off_backlight();
    if (s_overlay)
        lv_obj_delete(s_overlay);
    if (s_hint)
        lv_obj_delete(s_hint);
    s_overlay = nullptr;
    s_content = s_clock = s_date = s_prompt_text = nullptr;
    s_hint = nullptr;
    s_model.reset(0);
    s_model.set_foreground(false, 0);
}

extern "C" void ui_screensaver_lock_now(void)
{
    try {
        if (!s_model.active() && !s_exiting) enter_lockscreen();
    } catch (...) {
        stop_screensaver();
    }
}

extern "C" int ui_screensaver_filter_key(const struct key_item *item)
{
    try {
    if (!item)
        return 0;

    const uint32_t now = lv_tick_get();
    const bool pressed = item->key_state == KBD_KEY_PRESSED;
    const bool released = item->key_state == KBD_KEY_RELEASED;

    /* The exit animation owns every key until the working screen is back. */
    if (s_exiting)
        return 1;

    if (s_model.active()) {
        /* The lock screen owns every key while it is up. */
        const LockscreenDecision decision =
            s_lock.handle_key(item->key_code, pressed, released, now);
        if (decision.unlock) {
            stop_screensaver(true, true);
        } else if (decision.sleep) {
            sleep_lockscreen();
        } else if (decision.show_panel) {
            show_lockscreen_panel();
        }
        return decision.consumed ? 1 : 0;
    }

    /* Idle: watch the long-press gesture without consuming the key, so that a
     * short press still reaches the pages that use this key themselves. */
    s_model.note_activity(now);
    const ScreensaverHoldDecision hold =
        s_model.observe_hold_key(item->key_code, released, now);
    if (hold.hide_hint)
        launcher_toast().hide();
    update_timer_period();
    return 0;
    } catch (...) {
        stop_screensaver();
        return 0;
    }
}

extern "C" int ui_screensaver_is_active(void)
{
    return s_model.active() || s_exiting;
}

extern "C" void ui_screensaver_set_foreground(int foreground)
{
    try {
    stop_screensaver();
    s_model.set_foreground(foreground != 0, lv_tick_get());
    } catch (...) {
        stop_screensaver();
        s_model.set_foreground(false, 0);
    }
}

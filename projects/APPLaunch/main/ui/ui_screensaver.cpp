/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "ui_screensaver.h"
#include "ui_low_battery.h"

#include "cp0_lvgl_app.h"
#include "cp0_enum_cast.h"
#include "hal_lvgl_bsp.h"
#include "keyboard_input.h"
#include "launcher_media_controls.h"
#include "launcher_platform.hpp"
#include "launcher_toast.h"
#include "lvgl/lvgl.h"
#include "model/lockscreen_state_model.hpp"
#include "model/screensaver_model.hpp"
#include "model/screensaver_runtime_contract.hpp"
#include "ui_app_page.hpp"
#include "cp0_font_service.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <future>
#include <string>
#include <utility>

namespace {

constexpr uint32_t kIdleCheckMs = 500;
constexpr uint32_t kIdleMaxMs = 5000;
constexpr uint32_t kHoldPollMs = 100;
constexpr uint32_t kExitAnimationMs = 350;
/* Shown once the idle TAB hold matures past the model's hint delay. */
constexpr const char *kHoldHintText = "hold tab 3s to lock";

lv_obj_t *s_overlay = nullptr;
lv_obj_t *s_hint = nullptr;
lv_obj_t *s_content = nullptr;
lv_obj_t *s_clock = nullptr;
lv_obj_t *s_date = nullptr;
lv_obj_t *s_prompt_text = nullptr;
constexpr uint32_t kColorAccent = 0x3DDC84;
constexpr uint32_t kColorText = 0xE6E6E6;
constexpr uint32_t kColorDim = 0x5F7A66;
constexpr lv_coord_t kContentLeft = 16;
constexpr lv_coord_t kClockTop = 22;
constexpr lv_coord_t kDateTop = 52;
constexpr lv_coord_t kPromptTop = 94;
lv_timer_t *s_timer = nullptr;
ScreensaverModel s_model;
LockscreenStateModel s_lock;
bool s_exiting = false;
std::future<bool> s_audio_prepare_future;
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
ScreensaverPanel wake_panel()
{
    ScreensaverPanel panel = full_screen_panel();
    const int top = std::min<int>(AppPageRoot::kTopBarHeightPx, panel.height);
    panel.y = top;
    panel.height -= top;
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
}

void refresh_clock()
{
    if (!s_clock || !s_date) return;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (!localtime_r(&now, &local)) return;
    char clock_text[8];
    char date_text[24];
    std::strftime(clock_text, sizeof(clock_text), "%H:%M", &local);
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

    s_clock = content_label(s_content, kCp0ChromeFontSize * 2, kColorText);
    if (s_clock) lv_obj_set_pos(s_clock, kContentLeft, kClockTop);
    s_date = content_label(s_content, kCp0ChromeFontSize, kColorDim);
    if (s_date) lv_obj_set_pos(s_date, kContentLeft, kDateTop);

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
    if (lv_obj_t *cursor = lv_obj_create(prompt)) {
        lv_obj_remove_style_all(cursor);
        lv_obj_set_size(cursor, 7, 11);
        lv_obj_set_style_margin_left(cursor, 2, 0);
        lv_obj_set_style_bg_color(cursor, lv_color_hex(kColorAccent), 0);
        lv_obj_set_style_bg_opa(cursor, LV_OPA_COVER, 0);
    }
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

/* ---- lock-screen sounds ------------------------------------------------
 * The platform's system-sound player decodes each registered sound once, keeps
 * the decoded PCM and a warm engine, and plays it on its own worker thread.  So
 * the assets are registered by name once and then played through the by-name
 * signal: the unregistered fallback would instead re-open the audio device on
 * every play, which also swallows the start of a short sound while the sink
 * settles.  Registration appends after the platform's indexed sounds, so the
 * launcher's own startup/switch/enter slots are untouched. */
/* Last asset asked for, kept for diagnostics and for tests to observe the
 * request without racing the player's worker. */
const char *s_last_sound_asset = "";

// UI sounds are removed from this launcher build; lock-screen cues are silent.
void play_lockscreen_sound(const char *asset) noexcept
{
    if (asset) s_last_sound_asset = asset;
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

void suspend_system_sound() noexcept
{
    try {
        cp0_signal_audio_api({"SystemSoundSuspend"}, nullptr);
    } catch (...) {
    }
}

bool prepare_system_sound() noexcept
{
    int code = -1;
    try {
        cp0_signal_audio_api({"SystemSoundPrepare"},
                             [&](int result, std::string) { code = result; });
    } catch (...) {
    }
    return code == 0;
}

void start_system_sound_prepare() noexcept
{
    try {
        if (s_audio_prepare_future.valid())
            (void)s_audio_prepare_future.get();
        s_audio_prepare_future = std::async(
            std::launch::async, [] { return prepare_system_sound(); });
    } catch (...) {
    }
}

bool finish_system_sound_prepare() noexcept
{
    if (!s_audio_prepare_future.valid())
        return false;
    try {
        return s_audio_prepare_future.get();
    } catch (...) {
        return false;
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
    (void)finish_system_sound_prepare();
    release_screen_off_backlight();
    try {
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
        if (s_hint) lv_obj_delete(s_hint);
        s_hint = nullptr;
        s_exiting = false;
        s_model.deactivate();
        release_screen_off_backlight();
        update_timer_period();
    } catch (...) {
        s_overlay = nullptr;
        s_content = s_clock = s_date = s_prompt_text = nullptr;
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
    if (active)
        start_system_sound_prepare();

    /* Bring the panel back before the exit animation so wake-up is visible, and
     * drop the lock so the next entry starts black. */
    release_screen_off_backlight();
    hide_hint();
    hide_hold_hint();
    s_lock.reset(lv_tick_get());
    s_model.deactivate();
    update_timer_period();

    if (active && animated && s_overlay) {
        /* Slide the wallpaper away while keeping the page's status bar in place. */
        apply_panel(wake_panel());
        if (s_panel.height > 0) {
            cancel_exit_animation();
            s_exiting = true;
            lv_anim_t animation;
            lv_anim_init(&animation);
            lv_anim_set_var(&animation, s_overlay);
            lv_anim_set_values(&animation, s_panel.y, -s_panel.height);
            lv_anim_set_duration(&animation, kExitAnimationMs);
            lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
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

    suspend_system_sound();
    suspend_screen_off_backlight();
    play_lockscreen_sound("lock.mp3");

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
}

/* Back to (1) after the visible state timed out. */
void sleep_lockscreen()
{
    apply_panel(full_screen_panel());
    hide_hint();
    suspend_screen_off_backlight();
    play_lockscreen_sound("lock.mp3");
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
        else if (!s_panel.black)
            refresh_clock();
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
    (void)finish_system_sound_prepare();
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
            play_lockscreen_sound("unlock.mp3");
            stop_screensaver(true, true);
        } else if (decision.sleep) {
            sleep_lockscreen();
        } else {
            if (decision.blocked)
                play_lockscreen_sound("blocked.mp3");
            else if (decision.show_panel)
                play_lockscreen_sound("select.mp3");
            if (decision.show_panel) show_lockscreen_panel();
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

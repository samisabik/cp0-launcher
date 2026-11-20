/* SPDX-License-Identifier: MIT */
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../../main/ui/ui_low_battery.cpp"
#include "../../main/ui/ui_lockscreen.cpp"
#include "../../../../ext_components/cp0_lvgl/src/cp0_keyboard_queue.h"

extern "C" void test_keyboard_read(lv_indev_t *, lv_indev_data_t *);

/* ui_lockscreen.cpp announces the idle TAB hold through the shared launcher
 * toast.  This harness renders no toast, so record the transitions instead of
 * building the real object. */
static int hold_hint_shows;
static int hold_hint_hides;
static const char *hold_hint_text;

void LauncherToast::show(const char *text) noexcept { (void)text; }

void LauncherToast::show_persistent(const char *text, bool centered) noexcept
{
    (void)centered;
    hold_hint_text = text;
    ++hold_hint_shows;
}

void LauncherToast::hide() { ++hold_hint_hides; }

void LauncherToast::shutdown() {}

LauncherToast &launcher_toast()
{
    static LauncherToast toast;
    return toast;
}

static cp0_battery_info_t battery{};
static int shutdown_calls;
static int custom_events;
static int global_events;
static int previous_filter_calls;
static int battery_reads;
static int backlight_maximum = 255;
static int backlight_raw = 128;
static int backlight_suspends;
static int backlight_restores;
static bool fail_backlight_write;
static std::string lockscreen_timeout = "0";

decltype(cp0_signal_config_api) cp0_signal_config_api;
decltype(cp0_signal_audio_api) cp0_signal_audio_api;
decltype(cp0_signal_filesystem_api) cp0_signal_filesystem_api;
decltype(cp0_signal_settings_api) cp0_signal_settings_api;
decltype(cp0_signal_bq27220_api) cp0_signal_bq27220_api;

Cp0FontService::~Cp0FontService() = default;
lv_font_t *Cp0FontService::get_chrome(uint16_t) { return const_cast<lv_font_t *>(LV_FONT_DEFAULT); }
Cp0FontService &cp0_fonts()
{
    static Cp0FontService fonts;
    return fonts;
}
extern "C" int cp0_wifi_status_read(cp0_wifi_status_t *status) { *status = {}; return 0; }
extern "C" void cp0_lvgl_set_input_pump(int (*)(void)) {}

extern "C" cp0_battery_info_t cp0_battery_read() { ++battery_reads; return battery; }
extern "C" void cp0_system_shutdown() { ++shutdown_calls; }

static void set_battery(int soc, bool charging = false, bool valid = true)
{
    battery.soc = soc;
    battery.flags = charging ? 1 : 0;
    battery.valid = valid ? 1 : 0;
}

static void advance(uint32_t ms)
{
    lv_tick_inc(ms);
    launcher_battery_ui::timer_cb(launcher_battery_ui::warning_timer);
}

static bool visible()
{
    return launcher_battery_ui::overlay &&
        !lv_obj_has_flag(launcher_battery_ui::overlay, LV_OBJ_FLAG_HIDDEN);
}

static void key(uint32_t code, int state, bool consumed)
{
    key_item item{};
    item.key_code = code;
    item.key_state = state;
    const int custom_before = custom_events;
    const int global_before = global_events;
    assert(cp0_keyboard_queue_push(&item) == 0);
    lv_indev_data_t data{};
    test_keyboard_read(nullptr, &data);
    assert(custom_events == custom_before + (consumed ? 0 : 1));
    assert(global_events == global_before + (consumed ? 0 : 1));
    assert(consumed ? data.key == 0 : data.key != 0);
}

static int previous_filter(const key_item *)
{
    ++previous_filter_calls;
    return 0;
}

static void snapshot_if_requested(const char *variable = "BATTERY_UI_SNAPSHOT")
{
    const char *path = std::getenv(variable);
    if (!path) return;
    auto *buf = lv_snapshot_take(lv_layer_top(), LV_COLOR_FORMAT_RGB888);
    assert(buf);
    FILE *file = std::fopen(path, "wb");
    assert(file);
    std::fprintf(file, "P6\n%u %u\n255\n", buf->header.w, buf->header.h);
    for (unsigned y = 0; y < buf->header.h; ++y) {
        const auto *row = buf->data + y * buf->header.stride;
        for (unsigned x = 0; x < buf->header.w; ++x) {
            const unsigned char rgb[] = {row[x * 3 + 2], row[x * 3 + 1], row[x * 3]};
            std::fwrite(rgb, 1, 3, file);
        }
    }
    std::fclose(file);
    lv_draw_buf_destroy(buf);
}

/* Unlock the way a user does: any key brings the visible screen up, TAB arms the
 * unlock, ENTER confirms it and the panel slides away. */
static void unlock_lockscreen()
{
    assert(ui_lockscreen_is_active());
    assert(s_lock.state() == LockscreenState::Locked);

    key(KEY_ESC, KBD_KEY_PRESSED, true);            // (1) -> (2)
    assert(s_lock.state() == LockscreenState::PendingUnlock);
    key(KEY_ESC, KBD_KEY_RELEASED, true);
    key(KEY_TAB, KBD_KEY_PRESSED, true);            // (2) -> (3)
    assert(s_lock.state() == LockscreenState::Armed);
    key(KEY_TAB, KBD_KEY_RELEASED, true);

    key(KEY_ENTER, KBD_KEY_PRESSED, true);          // (3) -> unlock
    assert(s_exiting && ui_lockscreen_is_active() && !visible());
    advance(100);
    lv_anim_refr_now();
    assert(ui_lockscreen_is_active() && !visible());
    key(KEY_ENTER, KBD_KEY_RELEASED, true);
    advance(400);
    lv_anim_refr_now();
    assert(!ui_lockscreen_is_active());
    assert(lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN));
}

/* (2) -> (3) -> unlock, for callers that already reached (2). */
static void unlock_lockscreen_from_pending_unlock()
{
    assert(s_lock.state() == LockscreenState::PendingUnlock);
    key(KEY_TAB, KBD_KEY_PRESSED, true);
    assert(s_lock.state() == LockscreenState::Armed);
    key(KEY_TAB, KBD_KEY_RELEASED, true);
    key(KEY_ENTER, KBD_KEY_PRESSED, true);
    assert(s_exiting && ui_lockscreen_is_active());
    key(KEY_ENTER, KBD_KEY_RELEASED, true);
    advance(400);
    lv_anim_refr_now();
    assert(!ui_lockscreen_is_active());
}

static void test_hold_gesture_enters_lock()
{
    /* A healthy pack keeps the low-battery reminder quiet: while a reminder is
     * up its own filter owns every key, so it would swallow the gesture. */
    set_battery(80);
    advance(3000);
    assert(!visible());

    // The idle timeout stays off so only the gesture can lock the panel.
    lockscreen_timeout = "0";
    assert(!ui_lockscreen_is_active());
    backlight_raw = 128;
    backlight_suspends = 0;
    backlight_restores = 0;

    // Until the threshold matures the key still belongs to the page; the gesture
    // announces itself first without taking the key over.
    hold_hint_shows = 0;
    hold_hint_hides = 0;
    key(KEY_TAB, KBD_KEY_PRESSED, false);
    advance(499);
    ::timer_cb(s_timer);
    assert(hold_hint_shows == 0);
    advance(1);
    ::timer_cb(s_timer);
    assert(hold_hint_shows == 1);
    assert(std::strcmp(hold_hint_text, "hold tab 3s to lock") == 0);
    advance(2499);
    ::timer_cb(s_timer);
    assert(!ui_lockscreen_is_active());
    assert(backlight_raw == 128 && backlight_suspends == 0);
    assert(hold_hint_hides == 0);

    // Auto-repeat keeps the original window instead of restarting it.
    key(KEY_TAB, KBD_KEY_REPEATED, false);
    advance(1);
    ::timer_cb(s_timer);
    assert(ui_lockscreen_is_active());
    assert(s_lock.state() == LockscreenState::Locked);
    assert(s_panel.black);
    assert(backlight_raw == 0 && backlight_suspends == 1);
    assert(!s_model.hold_pending());
    // Entering the lock clears the persistent announcement.
    assert(hold_hint_hides == 1);
    assert(s_content && lv_obj_has_flag(s_content, LV_OBJ_FLAG_HIDDEN));
    assert(s_prompt_text && lv_label_get_text(s_prompt_text)[0] == '\0');

    /* The held key cannot walk the machine: its repeats and its release are not
     * fresh presses, so letting go leaves the panel black. */
    key(KEY_TAB, KBD_KEY_REPEATED, true);
    key(KEY_TAB, KBD_KEY_RELEASED, true);
    assert(s_lock.state() == LockscreenState::Locked);
    assert(s_panel.black && backlight_raw == 0);

    // The next fresh press brings the visible screen up.
    key(KEY_ESC, KBD_KEY_PRESSED, true);
    assert(s_lock.state() == LockscreenState::PendingUnlock);
    key(KEY_ESC, KBD_KEY_RELEASED, true);
    unlock_lockscreen_from_pending_unlock();
}

static void test_lockscreen_panel()
{
    // Keep the low-battery reminder out of the way.
    set_battery(80);
    advance(3000);
    assert(!visible());

    lv_display_t *display = lv_display_get_default();
    assert(display);
    const int width = lv_display_get_horizontal_resolution(display);
    const int height = lv_display_get_vertical_resolution(display);

    backlight_raw = 128;
    backlight_suspends = 0;
    backlight_restores = 0;
    lockscreen_timeout = "10";
    advance(10000);
    ::timer_cb(s_timer);

    /* (1) is pure black over the whole display, so it reads as a black screen on
     * every backend; the backlight going down is only a power optimisation. */
    assert(ui_lockscreen_is_active());
    assert(!visible());
    assert(s_lock.state() == LockscreenState::Locked);
    assert(s_panel.black);
    assert(s_panel.x == 0 && s_panel.y == 0);
    assert(s_panel.width == width && s_panel.height == height);
    assert(lv_color_eq(lv_obj_get_style_bg_color(s_overlay, LV_PART_MAIN), lv_color_black()));
    assert(backlight_raw == 0 && backlight_suspends == 1);
    assert(s_screen_off_backlight_raw == 128);
    // No artwork is painted, and no frames are driven for a static panel.
    assert(s_content && lv_obj_has_flag(s_content, LV_OBJ_FLAG_HIDDEN));
    assert(s_prompt_text && lv_label_get_text(s_prompt_text)[0] == '\0');

    /* (2) shows the status card over the whole display with the backlight
     * back, and asks for the first unlock step. */
    lockscreen_timeout = "0";
    key(KEY_ESC, KBD_KEY_PRESSED, true);
    assert(s_lock.state() == LockscreenState::PendingUnlock);
    assert(backlight_raw == 128 && backlight_restores == 1);
    assert(!s_panel.black);
    assert(s_panel.y == 0 && s_panel.height == height);
    assert(!lv_obj_has_flag(s_content, LV_OBJ_FLAG_HIDDEN));
    assert(std::strcmp(lv_label_get_text(s_prompt_text), "press tab to unlock") == 0);
    key(KEY_ESC, KBD_KEY_RELEASED, true);

    // (3) switches the hint to the confirmation step.
    key(KEY_TAB, KBD_KEY_PRESSED, true);
    assert(s_lock.state() == LockscreenState::Armed);
    assert(std::strcmp(lv_label_get_text(s_prompt_text), "press enter to unlock") == 0);
    key(KEY_TAB, KBD_KEY_RELEASED, true);

    // A key that is not the confirmation steps back to (2).
    key(KEY_TAB, KBD_KEY_PRESSED, true);
    assert(s_lock.state() == LockscreenState::PendingUnlock);
    assert(std::strcmp(lv_label_get_text(s_prompt_text), "press tab to unlock") == 0);
    key(KEY_TAB, KBD_KEY_RELEASED, true);

    /* The 10 s timeout drops the visible state back to the black screen and puts
     * the backlight down again. */
    const uint32_t before_sleep = lv_tick_get();
    advance(9999);
    ::timer_cb(s_timer);
    assert(s_lock.state() == LockscreenState::PendingUnlock);
    advance(1);
    ::timer_cb(s_timer);
    assert(s_lock.state() == LockscreenState::Locked);
    assert(s_panel.black);
    assert(backlight_raw == 0);
    assert(lv_tick_get() > before_sleep);
    assert(s_prompt_text && lv_label_get_text(s_prompt_text)[0] == '\0');

    // ...and the countdown starts over from the next press.
    key(KEY_ESC, KBD_KEY_PRESSED, true);
    assert(s_lock.state() == LockscreenState::PendingUnlock);
    key(KEY_ESC, KBD_KEY_RELEASED, true);
    advance(5000);
    ::timer_cb(s_timer);
    key(KEY_ENTER, KBD_KEY_PRESSED, true);
    key(KEY_ENTER, KBD_KEY_RELEASED, true);
    advance(9999);
    ::timer_cb(s_timer);
    assert(s_lock.state() == LockscreenState::PendingUnlock);
    advance(1);
    ::timer_cb(s_timer);
    assert(s_lock.state() == LockscreenState::Locked);

    // Full unlock slides the wallpaper away and hides the hint.
    key(KEY_ESC, KBD_KEY_PRESSED, true);
    key(KEY_ESC, KBD_KEY_RELEASED, true);
    key(KEY_TAB, KBD_KEY_PRESSED, true);
    key(KEY_TAB, KBD_KEY_RELEASED, true);
    key(KEY_ENTER, KBD_KEY_PRESSED, true);
    assert(s_exiting && !s_panel.black);
    assert(s_prompt_text && lv_label_get_text(s_prompt_text)[0] == '\0');
    key(KEY_ENTER, KBD_KEY_RELEASED, true);
    advance(400);
    lv_anim_refr_now();
    assert(!ui_lockscreen_is_active());
    assert(lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN));
}

static void test_unlock_restarts_idle_timeout()
{
    set_battery(80);
    advance(3000);
    assert(!visible());
    lockscreen_timeout = "10";
    backlight_raw = 128;

    for (bool poll_during_exit : {false, true}) {
        key(KEY_ESC, KBD_KEY_PRESSED, false);
        key(KEY_ESC, KBD_KEY_RELEASED, false);
        advance(10000);
        ::timer_cb(s_timer);
        assert(ui_lockscreen_is_active() && s_panel.black);

        // Keep auto-lock enabled after a long stay on the lock screen.
        advance(30000);
        key(KEY_ESC, KBD_KEY_PRESSED, true);
        key(KEY_ESC, KBD_KEY_RELEASED, true);
        key(KEY_TAB, KBD_KEY_PRESSED, true);
        key(KEY_TAB, KBD_KEY_RELEASED, true);
        key(KEY_ENTER, KBD_KEY_PRESSED, true);
        key(KEY_ENTER, KBD_KEY_RELEASED, true);
        assert(s_exiting);

        advance(100);
        if (poll_during_exit) ::timer_cb(s_timer);
        assert(s_exiting && !s_panel.black && backlight_raw == 128);
        lv_anim_refr_now();
        advance(400);
        lv_anim_refr_now();
        assert(!ui_lockscreen_is_active());

        // A timer firing immediately after the animation must not relock.
        ::timer_cb(s_timer);
        assert(!ui_lockscreen_is_active());
        assert(lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN));
        assert(backlight_raw == 128);
        advance(9999);
        ::timer_cb(s_timer);
        assert(!ui_lockscreen_is_active());
        advance(1);
        ::timer_cb(s_timer);
        assert(ui_lockscreen_is_active() && s_panel.black);

        unlock_lockscreen();
        ::timer_cb(s_timer);
        assert(!ui_lockscreen_is_active());
        key(KEY_ESC, KBD_KEY_PRESSED, false);
        key(KEY_ESC, KBD_KEY_RELEASED, false);
    }
    lockscreen_timeout = "0";
}

static void test_lockscreen_black_is_independent_of_backlight()
{
    set_battery(80);
    advance(3000);
    assert(!visible());

    /* The black screen must not depend on the backlight: the simulator and web
     * backends accept the write and dim nothing, so a panel that relied on
     * it would be fully visible there.  With the write failing outright the
     * panel is still pure black and the backlight is left alone. */
    fail_backlight_write = true;
    backlight_raw = 128;
    lockscreen_timeout = "10";
    advance(10000);
    ::timer_cb(s_timer);

    assert(ui_lockscreen_is_active());
    assert(s_lock.state() == LockscreenState::Locked);
    assert(s_screen_off_backlight_raw < 0);
    assert(backlight_raw == 128);
    assert(s_panel.black);
    assert(s_panel.x == 0 && s_panel.y == 0);
    assert(s_panel.width == lv_display_get_horizontal_resolution(lv_display_get_default()));
    assert(s_panel.height == lv_display_get_vertical_resolution(lv_display_get_default()));
    assert(lv_color_eq(lv_obj_get_style_bg_color(s_overlay, LV_PART_MAIN), lv_color_black()));

    // Unlocking still works and reveals the wallpaper.
    fail_backlight_write = false;
    lockscreen_timeout = "0";
    key(KEY_ESC, KBD_KEY_PRESSED, true);
    key(KEY_ESC, KBD_KEY_RELEASED, true);
    assert(s_lock.state() == LockscreenState::PendingUnlock);
    assert(!s_panel.black && s_panel.y == 0);
    unlock_lockscreen_from_pending_unlock();
}

static void test_lockscreen_battery_interaction()
{
    using namespace launcher_battery_ui;
    shutdown_warning();
    set_battery(30);
    init_warning();
    assert(!visible());
    enter_lockscreen();
    assert(ui_lockscreen_is_active());

    // An unseen reminder is deferred until the working screen is fully visible.
    const int reads_before = battery_reads;
    set_battery(20);
    advance(3000);
    assert(battery_reads > reads_before);
    assert(flow.warning() == LowBatteryWarning::Reminder && !visible());
    update_warning(battery);
    assert(!visible());
    advance(60000);
    assert(!visible());
    assert(lv_obj_get_child(lv_layer_top(), -1) == s_overlay);
    snapshot_if_requested("BATTERY_LOCKSCREEN_SNAPSHOT");
    unlock_lockscreen();
    assert(visible());
    snapshot_if_requested("BATTERY_WAKE_SNAPSHOT");
    key(KEY_ENTER, KBD_KEY_PRESSED, true);
    key(KEY_ENTER, KBD_KEY_RELEASED, true);
    assert(!visible());
    enter_lockscreen();
    advance(3000);
    unlock_lockscreen();
    assert(!visible());

    // Charging and unplugging still rearm the reminder under the lock screen.
    enter_lockscreen();
    set_battery(15, true);
    advance(3000);
    set_battery(15);
    advance(3000);
    assert(flow.warning() == LowBatteryWarning::Reminder && !visible());
    unlock_lockscreen();
    assert(visible());

    // Hidden warnings do not flash, steal the foreground, or pause a countdown.
    set_battery(4);
    advance(3000);
    assert(visible());
    enter_lockscreen();
    assert(!visible());
    const bool hidden_frame = frame_bright;
    advance(1000);
    assert(!visible() && frame_bright == hidden_frame);
    set_battery(3);
    advance(3000);
    assert(flow.warning() == LowBatteryWarning::ShutdownCountdown && !visible());
    advance(6000);
    unlock_lockscreen();
    assert(visible());
    assert(flow.seconds_until_shutdown(lv_tick_get()) == 9);
    assert(std::strcmp(lv_label_get_text(countdown_label), "power off in 9s") == 0);
    enter_lockscreen();
    assert(!visible());
    const int shutdown_before = shutdown_calls;
    advance(8500);
    assert(shutdown_calls == shutdown_before + 1 && !visible());
    assert(lv_obj_get_child(lv_layer_top(), -1) == s_overlay);
    advance(3000);
    assert(shutdown_calls == shutdown_before + 1);

    // All cancellation paths continue to apply even with every warning hidden.
    for (int recovery : {0, 1, 2, 3}) {
        set_battery(30);
        advance(3000);
        set_battery(3);
        advance(3000);
        advance(12000);
        if (recovery == 0) set_battery(3, true);
        else if (recovery == 1) set_battery(3, false, false);
        else if (recovery == 2) set_battery(4);
        else set_battery(25);
        const int shutdown_before_recovery = shutdown_calls;
        advance(3000);
        assert(!visible() && shutdown_calls == shutdown_before_recovery);
        assert(!flow.shutdown_due(lv_tick_get()));
        unlock_lockscreen();
        assert(visible() == (recovery == 2));
        enter_lockscreen();
    }
    stop_lockscreen();
    shutdown_warning();
}

int main()
{
    using namespace launcher_battery_ui;
    lv_init();
    auto *display = lv_display_create(320, 170);
    assert(display);
    LV_EVENT_KEYBOARD = lv_event_register_id();
    lv_obj_add_event_cb(lv_screen_active(), [](lv_event_t *) { ++custom_events; },
                       static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), nullptr);
    cp0_keyboard_set_global_key_handler([](const key_item *) { ++global_events; });
    cp0_keyboard_set_key_filter(previous_filter);
    cp0_signal_config_api.append([](std::list<std::string>, std::function<void(int, std::string)> reply) {
        if (reply) reply(0, lockscreen_timeout);
    });
    cp0_signal_audio_api.append([](std::list<std::string>, std::function<void(int, std::string)> reply) {
        if (reply) reply(0, "");
    });
    cp0_signal_filesystem_api.append([](std::list<std::string> arguments, std::function<void(int, std::string)> reply) {
        (void)arguments;
        if (reply) reply(0, "/missing-test-image.png");
    });
    cp0_signal_settings_api.append([](std::list<std::string> arguments,
                                      std::function<void(int, std::string)> reply) {
        assert(!arguments.empty());
        const std::string command = arguments.front();
        if (command == "BacklightMax") {
            if (reply) reply(0, std::to_string(backlight_maximum));
        } else if (command == "BacklightRead") {
            if (reply) reply(0, std::to_string(backlight_raw));
        } else if (command == "BacklightWrite") {
            if (fail_backlight_write) {
                if (reply) reply(-1, "backlight write failed");
            } else {
                backlight_raw = std::stoi(*std::next(arguments.begin()));
                if (backlight_raw == 0) ++backlight_suspends;
                else ++backlight_restores;
                if (reply) reply(0, std::to_string(backlight_raw));
            }
        } else {
            assert(false);
        }
    });
    ui_lockscreen_init();
    assert(s_timer && s_overlay);

    set_battery(20);
    init_warning();
    assert(visible());
    assert(std::strstr(lv_label_get_text(message_label), "enter to close"));
    snapshot_if_requested();
    advance(60000);
    assert(visible());
    set_battery(19);
    advance(3000);
    assert(std::strcmp(lv_label_get_text(countdown_label), "battery 19%") == 0);
    assert(lv_obj_has_flag(tint, LV_OBJ_FLAG_HIDDEN));
    key(KEY_ESC, KBD_KEY_PRESSED, true);
    key(KEY_ESC, KBD_KEY_RELEASED, true);
    assert(visible());
    key(KEY_ENTER, KBD_KEY_REPEATED, true);
    key(KEY_ENTER, KBD_KEY_RELEASED, true);
    assert(visible());

    lockscreen_timeout = "10";
    advance(10000);
    ::timer_cb(s_timer);
    assert(ui_lockscreen_is_active() && !visible());
    lockscreen_timeout = "0";
    unlock_lockscreen();
    assert(visible());
    key(KEY_DOWN, KBD_KEY_PRESSED, true);
    key(KEY_ENTER, KBD_KEY_PRESSED, true);
    assert(!visible());
    key(KEY_ENTER, KBD_KEY_REPEATED, true);
    key(KEY_ENTER, KBD_KEY_RELEASED, true);
    key(KEY_DOWN, KBD_KEY_REPEATED, true);
    key(KEY_DOWN, KBD_KEY_RELEASED, true);
    key(KEY_ENTER, KBD_KEY_PRESSED, false);
    key(KEY_ENTER, KBD_KEY_RELEASED, false);
    assert(previous_filter_calls == 2);
    advance(60000);
    assert(!visible());

    set_battery(15, true);
    advance(3000);
    set_battery(15);
    advance(3000);
    assert(visible());
    key(KEY_KPENTER, KBD_KEY_PRESSED, true);
    assert(!visible());
    key(KEY_KPENTER, KBD_KEY_RELEASED, true);
    advance(3000);
    assert(!visible());

    set_battery(10, true);
    advance(3000);
    set_battery(10);
    advance(3000);
    key(KEY_ENTER, KBD_KEY_REPEATED, true);
    set_battery(10, false, false);
    advance(3000);
    assert(!visible());
    key(KEY_ENTER, KBD_KEY_RELEASED, true);
    set_battery(10);
    advance(3000);
    assert(!visible());

    set_battery(4);
    advance(3000);
    assert(visible());
    assert(!lv_obj_has_flag(tint, LV_OBJ_FLAG_HIDDEN) && frame_bright);
    assert(lv_obj_get_child_count(tint) == 4);
    advance(500);
    assert(!frame_bright);
    key(KEY_ENTER, KBD_KEY_PRESSED, false);
    key(KEY_ENTER, KBD_KEY_RELEASED, false);
    assert(visible());
    advance(3000);

    set_battery(3);
    advance(3000);
    assert(std::strcmp(lv_label_get_text(countdown_label), "power off in 15s") == 0);
    advance(12000);
    set_battery(3, true);
    advance(3000);
    assert(shutdown_calls == 0 && !visible());
    set_battery(3);
    advance(3000);
    advance(15000);
    assert(shutdown_calls == 1);
    advance(3000);
    assert(shutdown_calls == 1);

    set_battery(10);
    advance(3000);
    lv_obj_delete(message_label);
    advance(250);
    assert(message_label && visible());
    test_hold_gesture_enters_lock();
    test_lockscreen_panel();
    test_unlock_restarts_idle_timeout();
    test_lockscreen_black_is_independent_of_backlight();
    shutdown_warning();
    assert(!overlay && !warning_timer);
    assert(cp0_keyboard_get_key_filter() == previous_filter);
    test_lockscreen_battery_interaction();
    ui_lockscreen_deinit();
    cp0_keyboard_set_key_filter(nullptr);
    lv_display_delete(display);
    lv_deinit();
    std::puts("Battery UI and keyboard dispatch checks passed");
}

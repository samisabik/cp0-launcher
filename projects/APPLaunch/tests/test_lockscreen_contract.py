# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
#
# SPDX-License-Identifier: MIT

"""Wiring contract for the lock screen.

The state machine itself is covered by test_lockscreen_state_model.cpp, the panel
and backlight behaviour by the low_battery_ui harness (which drives the real
ui_lockscreen.cpp), and the non-persisting backlight by
test_launcher_media_controls.cpp.  This only pins the wiring that is easy to lose
in a refactor.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LOCK = (ROOT / "main/ui/model/lockscreen_state_model.hpp").read_text()
SOURCE = (ROOT / "main/ui/ui_lockscreen.cpp").read_text()
CONTROLS = (ROOT / "main/ui/launcher_media_controls.cpp").read_text()
CONTROLS_HEADER = (ROOT / "main/ui/launcher_media_controls.h").read_text()
PAGE_SHELL = (ROOT / "../../ext_components/cp0_lvgl/include/ui_app_page.hpp").read_text()

# The lock states, their keys, and the idle timeout live in the model.
assert "enum class LockscreenState" in LOCK
assert "PendingUnlock" in LOCK and "Armed" in LOCK
assert "TimeoutMs = 10000" in LOCK
assert "KEY_TAB" in LOCK
assert "KEY_ENTER" in LOCK and "KEY_KPENTER" in LOCK

# Both entry points (the idle timeout and the long-press gesture) share one path,
# so the two can no longer drift apart.
assert "void enter_lockscreen()" in SOURCE
assert SOURCE.count("enter_lockscreen();") >= 2

# While the lock screen is up it owns every key and decides when the
# panel sleeps or unlocks.
assert "s_lock.handle_key(item->key_code, pressed, released, now)" in SOURCE
assert "s_lock.poll(now)" in SOURCE
assert "decision.unlock" in SOURCE
assert "show_lockscreen_panel()" in SOURCE
assert "sleep_lockscreen()" in SOURCE

# The long-press gesture stays observation-only while the lock screen is idle, and
# a pending hold is polled faster than the idle check.
assert "s_model.observe_hold_key(item->key_code, released, now)" in SOURCE
assert "s_model.poll_hold(now)" in SOURCE
assert "kHoldPollMs = 100" in SOURCE

# The gesture announces itself through the shared launcher toast before it locks,
# and clears that announcement again as soon as the gesture ends.
assert "launcher_toast().show_persistent(kHoldHintText)" in SOURCE
assert '"hold tab 3s to lock"' in SOURCE
assert "hold.hide_hint" in SOURCE
assert "hold.fire" in SOURCE

# The visible lock states cover the whole display, top bar included: the card
# carries the time and status, and the simulation fills the screen.
assert "kTopBarHeightPx" in PAGE_SHELL
assert "refresh_status(true)" in SOURCE

# The locked state paints pure black over the whole display, so the black screen
# never depends on the backlight actually dimming: the simulator and web
# backends accept the write and dim nothing.
assert "LockscreenPanel full_screen_panel()" in SOURCE
assert "apply_panel(full_screen_panel())" in SOURCE

# The visible states show a clock and a prompt line with the step hint; no wallpaper.
assert "LockscreenPanel wake_panel()" in SOURCE
assert "apply_panel(wake_panel())" in SOURCE
assert "press tab to unlock" in SOURCE
assert "press enter to unlock" in SOURCE
assert "show_hint(" in SOURCE and "hide_hint()" in SOURCE
assert "refresh_clock()" in SOURCE

# Unlocking slides the wallpaper away, restoring the backlight first.
assert "lv_anim_set_values(&animation, s_panel.y, -s_panel.height)" in SOURCE
assert "release_screen_off_backlight();" in SOURCE

assert "std::thread" not in SOURCE

# The panel is driven dark on entry and restored on every exit path, including
# teardown and the object-delete callbacks.
assert "suspend_screen_off_backlight()" in SOURCE
assert SOURCE.count("release_screen_off_backlight()") >= 4

# Suspending must not persist the brightness, or unlocking restores the wrong
# value; the normal step-based control can never express zero.
assert "int suspend_backlight();" in CONTROLS_HEADER
assert "void restore_backlight(int raw);" in CONTROLS_HEADER
assert "write_backlight_raw(0)" in CONTROLS
assert "if (raw <= 0) return;" in CONTROLS

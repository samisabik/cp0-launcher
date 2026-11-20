/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "ui.h"
#include "launcher_media_controls.h"
#include "launcher_ui_app_page.hpp"
#include "ui_low_battery.h"
#include "ui_global_hint.h"
#include "ui_loading.h"
#include "ui_lockscreen.h"
#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "settings/settings_prefs.hpp"

#if LV_COLOR_DEPTH != 16
    #error "The launcher draws RGB565 and needs LV_COLOR_DEPTH 16"
#endif

std::unique_ptr<LauncherUiRuntime> home;

namespace launcher_ui {

void init()
{
    if (home)
        return;
    // The saved accent has to be in place before the first widget is built.
    cp0::ui::set_palette(cp0::theme::accent_palette(
        cp0::theme::kAccentPresets[static_cast<std::size_t>(cp0::theme::accent_index(
                                       settings_prefs::get_int(cp0::theme::kAccentConfigKey, 0)))]
            .color));
    /* Cache the system volume once here so the shortcut OSD and
     * Settings -> Speaker share one value for the whole launcher session. */
    launcher_media_controls::start_volume_cache();
    cp0_keyboard_set_global_key_handler(ui_global_hint::on_key);
    home = std::make_unique<LauncherUiRuntime>();
    home->start();
    launcher_battery_ui::init_warning();
}

void deinit()
{
    cp0_keyboard_set_global_key_handler(nullptr);
    launcher_media_controls::stop_volume_cache();
    ui_global_hint::shutdown();
    launcher_battery_ui::shutdown_warning();
    ui_loading::shutdown();
    ui_lockscreen_deinit();
    home.reset();
}

} // namespace launcher_ui

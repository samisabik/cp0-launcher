/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "launcher_ui_runtime.h"

#include "launch.h"
#include "ui_launch_page.h"

#include "hal_lvgl_bsp.h"

void LauncherUiRuntime::create_display()
{
    lv_disp_t *display = lv_disp_get_default();
    lv_theme_t *theme = lv_theme_default_init(display, lv_palette_main(LV_PALETTE_BLUE),
                                              lv_palette_main(LV_PALETTE_RED), false, LV_FONT_DEFAULT);
    lv_disp_set_theme(display, theme);
}

void LauncherUiRuntime::build_launcher_home()
{
    launch_page_->create_screen();
    launch_->bind_ui();
    launch_page_->init_input_group();
}

LauncherUiRuntime::LauncherUiRuntime()
{
    create_display();

    launch_ = std::make_unique<Launch>();
    launch_page_ = std::make_shared<UILaunchPage>(launch_.get());
    launch_->set_launch_page(launch_page_);
}

LauncherUiRuntime::~LauncherUiRuntime() = default;

void LauncherUiRuntime::start()
{
    build_launcher_home();
    launch_page_->show_home_screen();
}

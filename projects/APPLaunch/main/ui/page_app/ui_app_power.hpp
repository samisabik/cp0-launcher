/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "cp0_theme.hpp"

#include <array>
#include <cstdint>

// ============================================================
//  Power  UIPowerPage
//  A quick shutdown/reboot shortcut for the home screen's system group.
//  Reuses the settings power overlay, so the notice and the actual
//  poweroff/reboot behave exactly as they do from Settings.
//
//  Keys:
//    UP / DOWN   select shutdown or reboot
//    ENTER       run the selected action
//    ESC         home
// ============================================================
class UIPowerPage : public AppPage
{
public:
    UIPowerPage();
    ~UIPowerPage() override;

private:
    static constexpr int kRows = 2;

    struct Row {
        cp0::ui::Panel panel;
        lv_obj_t *label = nullptr;
    };

    int selected_ = 0;
    std::array<Row, kRows> rows_{};
    lv_obj_t *hint_ = nullptr;

    void create_ui();
    void render();
    void activate();
    void handle_key(uint32_t key);
    static void event_cb(lv_event_t *event) noexcept;
};

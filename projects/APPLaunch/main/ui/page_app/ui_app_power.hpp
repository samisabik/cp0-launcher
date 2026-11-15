/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"

#include <cstdint>

// ============================================================
//  Power  UIPowerPage
//  Clean shutdown before the hardware power switch is flipped.
//  ENTER asks for confirmation, ENTER again shows the instructions, then
//  runs the platform shutdown (backlight off, systemctl poweroff). ESC goes
//  back to the home screen.
// ============================================================
class UIPowerPage : public AppPage
{
public:
    UIPowerPage();
    ~UIPowerPage() override;

private:
    enum class State { Ask, Confirm, ShuttingDown };

    State state_ = State::Ask;
    lv_obj_t *title_ = nullptr;
    lv_obj_t *detail_ = nullptr;
    lv_obj_t *key_ = nullptr;
    lv_obj_t *word_ = nullptr;
    lv_obj_t *key2_ = nullptr;
    lv_obj_t *word2_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    void create_ui();
    void render();
    void handle_key(uint32_t key);
    static void event_cb(lv_event_t *event) noexcept;
    static void shutdown_cb(lv_timer_t *timer) noexcept;
};

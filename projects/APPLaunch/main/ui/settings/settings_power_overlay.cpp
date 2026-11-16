/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "settings_power_overlay.hpp"

#include "cp0_theme.hpp"
#include "hal_lvgl_bsp.h"
#include "keyboard_input.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace {

using Action = settings_t12b::boot_actions::Action;

// Long enough to read the notice before the shutdown request darkens the panel.
constexpr uint32_t kNoticeMs = 2000;
constexpr uint32_t kPollMs = 100;
constexpr uint32_t kRefusedMs = 8000;

struct Result {
    std::mutex mutex;
    bool done = false;
    int code = -1;
    std::string reason;
};

enum class Phase { Notice, Waiting, Accepted, Refused };

struct Overlay {
    Action action = Action::Shutdown;
    Phase phase = Phase::Notice;
    lv_obj_t *root = nullptr;
    lv_obj_t *headline = nullptr;
    lv_obj_t *detail = nullptr;
    lv_obj_t *footer = nullptr;
    lv_timer_t *timer = nullptr;
    uint32_t refused_at = 0;
    std::shared_ptr<Result> result = std::make_shared<Result>();
    cp0_keyboard_key_filter_t previous_filter = nullptr;
};

Overlay *g_overlay = nullptr;
std::atomic_bool g_refused{false};
std::atomic_bool g_dismiss{false};

// The notice owns the keyboard: nothing underneath may react while the
// request is in flight.  Once refused, any fresh press hands the screen back.
int overlay_key_filter(const struct key_item *item)
{
    if (item && item->key_state == KBD_KEY_PRESSED && g_refused.load()) g_dismiss.store(true);
    return 1;
}

void close_overlay()
{
    Overlay *overlay = g_overlay;
    if (!overlay) return;
    g_overlay = nullptr;
    if (cp0_keyboard_get_key_filter() == overlay_key_filter) cp0_keyboard_set_key_filter(overlay->previous_filter);
    if (overlay->timer) lv_timer_delete(overlay->timer);
    if (overlay->root) lv_obj_delete(overlay->root);
    g_refused.store(false);
    g_dismiss.store(false);
    delete overlay;
}

void start_request(Overlay *overlay)
{
    const char *command = settings_t12b::boot_actions::process_command(
        overlay->action == Action::Reboot ? settings_t12b::boot_actions::Operation::Reboot
                                          : settings_t12b::boot_actions::Operation::Shutdown);
    auto result = overlay->result;
    try {
        std::thread([result, command = std::string(command ? command : "")]() {
            int code = -1;
            std::string reason = "power service unavailable";
            try {
                cp0_signal_process_api({command}, [&](int result_code, std::string data) {
                    code = result_code;
                    reason = std::move(data);
                });
            } catch (...) {
            }
            std::lock_guard<std::mutex> lock(result->mutex);
            result->code = code;
            result->reason = std::move(reason);
            result->done = true;
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lock(result->mutex);
        result->reason = "cannot start the request";
        result->done = true;
    }
}

void show_refused(Overlay *overlay, const std::string &reason)
{
    const bool reboot = overlay->action == Action::Reboot;
    cp0::ui::set_text(overlay->headline, reboot ? "reboot refused" : "shutdown refused", cp0::theme::kDanger);
    const std::string detail = reason.empty() ? std::string("the system did not accept the request") : cp0::ui::lower(reason);
    cp0::ui::set_text(overlay->detail, detail.c_str(), cp0::theme::kText);
    cp0::ui::set_footer(overlay->footer, {{"any key", "back"}});
    overlay->phase = Phase::Refused;
    overlay->refused_at = lv_tick_get();
    g_refused.store(true);
}

void overlay_timer_cb(lv_timer_t *timer)
{
    Overlay *overlay = g_overlay;
    if (!overlay || overlay->timer != timer) return;
    switch (overlay->phase) {
    case Phase::Notice:
        overlay->phase = Phase::Waiting;
        lv_timer_set_period(timer, kPollMs);
        start_request(overlay);
        return;
    case Phase::Waiting: {
        int code = -1;
        std::string reason;
        {
            std::lock_guard<std::mutex> lock(overlay->result->mutex);
            if (!overlay->result->done) return;
            code = overlay->result->code;
            reason = overlay->result->reason;
        }
        if (code == 0) {
            overlay->phase = Phase::Accepted;
            cp0::ui::set_footer_message(overlay->footer,
                                        overlay->action == Action::Reboot ? "restarting" : "powering off",
                                        cp0::theme::kIdle);
            lv_timer_pause(timer);
            return;
        }
        show_refused(overlay, reason);
        return;
    }
    case Phase::Refused:
        if (g_dismiss.load() || lv_tick_elaps(overlay->refused_at) >= kRefusedMs) close_overlay();
        return;
    case Phase::Accepted:
        return;
    }
}

} // namespace

void settings_power_request(Action action)
{
    if (g_overlay) return;
    lv_obj_t *layer = lv_layer_top();
    if (!layer) return;

    auto *overlay = new Overlay();
    overlay->action = action;
    overlay->root = cp0::ui::box(layer, 0, 0, cp0::theme::kScreenW, cp0::theme::kScreenH, cp0::theme::kBg);
    if (!overlay->root) {
        delete overlay;
        return;
    }
    g_overlay = overlay;
    g_refused.store(false);
    g_dismiss.store(false);

    cp0::ui::dot_grid(overlay->root, 0, cp0::theme::kBodyTop, cp0::theme::kScreenW, cp0::theme::kBodyH);
    cp0::ui::title_row(overlay->root, "settings/system");
    const cp0::ui::Panel panel = cp0::ui::panel(overlay->root, 16, 36, cp0::theme::kScreenW - 32, 92);
    cp0::ui::set_panel_focus(panel, true, action == Action::Reboot ? cp0::theme::kAccent : cp0::theme::kWarn);
    const bool reboot = action == Action::Reboot;
    overlay->headline = cp0::ui::label(panel.box, 12, 12, cp0::theme::kText, true);
    cp0::ui::set_text(overlay->headline, reboot ? "rebooting" : "shutting down", cp0::theme::kText);
    overlay->detail = cp0::ui::label(panel.box, 12, 46, cp0::theme::kIdle);
    if (overlay->detail) {
        lv_obj_set_width(overlay->detail, cp0::theme::kScreenW - 32 - 24);
        lv_label_set_long_mode(overlay->detail, LV_LABEL_LONG_WRAP);
        lv_label_set_text(overlay->detail,
                          reboot ? "the device restarts in a moment" : "wait 10 s, then flip the power switch");
    }
    overlay->footer = cp0::ui::footer(overlay->root);
    cp0::ui::set_footer_message(overlay->footer, reboot ? "requesting reboot" : "requesting power off",
                                cp0::theme::kIdle);

    overlay->previous_filter = cp0_keyboard_get_key_filter();
    cp0_keyboard_set_key_filter(overlay_key_filter);
    overlay->timer = lv_timer_create(overlay_timer_cb, kNoticeMs, nullptr);
    if (!overlay->timer) {
        close_overlay();
        return;
    }
    lv_obj_invalidate(overlay->root);
}

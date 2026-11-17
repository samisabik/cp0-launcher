/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "settings_system_page.hpp"

#include "cp0_theme.hpp"
#include "hal_lvgl_bsp.h"
#include "settings_system_model.hpp"

#include <cstdint>
#include <list>
#include <string>
#include <utility>

namespace {

constexpr uint32_t kPollMs = 500;

// apt-get update + upgrade. The launcher is installed from source and its
// package is held, so the upgrade never touches it.
constexpr const char *kActionLabel = "update and upgrade packages";

// The osinfo service answers these requests inline, so they are safe to call
// from the LVGL thread; the work itself runs in the service's job threads.
int osinfo_call(std::list<std::string> arguments, std::string &payload)
{
    int result = -1;
    try {
        cp0_signal_osinfo_api(std::move(arguments), [&](int code, std::string data) {
            result = code;
            payload = std::move(data);
        });
    } catch (...) {
        payload = "osinfo service unavailable";
    }
    return result;
}

class LvSettingUpdatesPage3 : public DComponens::LvglComponensBase {
public:
    LvSettingUpdatesPage3(lv_obj_t *parent, std::function<void()> back_callback)
    {
        LeaveSelfPage = std::move(back_callback);
        create_ui(parent);
    }

    ~LvSettingUpdatesPage3() override
    {
        // The upgrade keeps running when the page closes; dpkg must not be
        // interrupted. Reopening the page and pressing enter follows it again.
        if (timer_) lv_timer_delete(timer_);
        timer_ = nullptr;
        if (ComponensObj) {
            lv_obj_delete(ComponensObj);
            ComponensObj = nullptr;
        }
    }

    void LoadNextPage() override {}

    void LeaveNextPage() override
    {
        if (LeaveSelfPage) LeaveSelfPage();
    }

    void create_ui(lv_obj_t *parent) override
    {
        if (!parent) return;
        ComponensObj = cp0::ui::box(parent, 0, 0, cp0::theme::kScreenW, cp0::theme::kScreenH, cp0::theme::kBg);
        if (!ComponensObj) return;
        DComponens::lvgl_bind_event(ComponensObj, LV_EVENT_KEY, nullptr,
                                    [this](lv_event_t *event) { handle_key_event(event); });

        title_ = cp0::ui::title_row(ComponensObj, "settings/system/updates");
        actions_ = cp0::ui::panel(ComponensObj, cp0::theme::kPadX, 24, cp0::theme::kScreenW - 2 * cp0::theme::kPadX,
                                  21);
        row_.band = cp0::ui::box(actions_.box, 2, 3, cp0::theme::kScreenW - 2 * cp0::theme::kPadX - 4, 15,
                                 cp0::theme::kSelect);
        row_.text = cp0::ui::label(actions_.box, 8, 4, cp0::theme::kIdle);

        progress_ = cp0::ui::segbar(ComponensObj, cp0::theme::kPadX, 54, cp0::theme::kScreenW - 2 * cp0::theme::kPadX,
                                    4, cp0::theme::kBg);
        message_ = cp0::ui::label(ComponensObj, cp0::theme::kPadX, 64, cp0::theme::kIdle);
        if (message_) {
            lv_obj_set_width(message_, cp0::theme::kScreenW - 2 * cp0::theme::kPadX);
            lv_label_set_long_mode(message_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        }
        footer_ = cp0::ui::footer(ComponensObj);

        message_text_ = "ent runs apt update and apt upgrade";
        render();
    }

private:
    struct Row {
        lv_obj_t *band = nullptr;
        lv_obj_t *text = nullptr;
    };

    void render()
    {
        const bool busy = !job_id_.empty();
        cp0::ui::set_text(row_.text, kActionLabel, busy ? cp0::theme::kDim : cp0::theme::kText);
        cp0::ui::set_segbar(progress_, static_cast<float>(progress_value_) / 100.0f,
                            message_color_ == cp0::theme::kDanger ? cp0::theme::kDanger : cp0::theme::kAccent);
        cp0::ui::set_text(message_, message_text_.c_str(), message_color_);
        cp0::ui::set_text(title_.status, busy ? "working" : "", cp0::theme::kWarn);
        if (busy)
            cp0::ui::set_footer(footer_, {{"esc", "back"}});
        else
            cp0::ui::set_footer(footer_, {{"ent", "run"}, {"esc", "back"}});
    }

    void set_message(std::string text, uint32_t color)
    {
        message_text_ = std::move(text);
        message_color_ = color;
    }

    void start()
    {
        if (!job_id_.empty()) return;
        std::string id;
        if (osinfo_call({"SystemUpgradeStart"}, id) != 0 || id.empty()) {
            set_message("could not start: " + cp0::ui::lower(id.empty() ? std::string("no job id") : id),
                        cp0::theme::kDanger);
            render();
            return;
        }
        job_id_ = id;
        progress_value_ = 0;
        set_message("refreshing package lists", cp0::theme::kIdle);
        if (!timer_) timer_ = lv_timer_create(poll_cb, kPollMs, this);
        render();
    }

    static void poll_cb(lv_timer_t *timer)
    {
        auto *self = static_cast<LvSettingUpdatesPage3 *>(lv_timer_get_user_data(timer));
        if (self) self->poll();
    }

    void poll()
    {
        if (job_id_.empty()) return;
        std::string state;
        if (osinfo_call({"UpdateJobStatus", job_id_}, state) != 0) {
            finish();
            set_message("update job lost", cp0::theme::kDanger);
            render();
            return;
        }
        const settings_system::UpdateStatusInfo info = settings_system::parse_update_status(state);
        if (!info.terminal) {
            if (info.progress >= 0) progress_value_ = info.progress;
            const char *what = info.stage == "downloading" ? "downloading packages"
                               : info.stage == "installing" ? "installing"
                                                            : "refreshing package lists";
            set_message(std::string(what) + (info.progress >= 0 ? " " + std::to_string(info.progress) + "%" : ""),
                        cp0::theme::kIdle);
            render();
            return;
        }

        finish();
        if (state.rfind("succeeded:", 0) == 0) {
            progress_value_ = 100;
            set_message("up to date, " + state.substr(10), cp0::theme::kAccent);
        } else {
            // failed:<reason>:<code>
            std::string reason = state.rfind("failed:", 0) == 0 ? state.substr(7) : state;
            const std::size_t code = reason.rfind(':');
            if (code != std::string::npos) reason.resize(code);
            set_message("failed: " + cp0::ui::lower(reason), cp0::theme::kDanger);
        }
        render();
    }

    void finish()
    {
        job_id_.clear();
        if (timer_) lv_timer_delete(timer_);
        timer_ = nullptr;
    }

    void handle_key_event(lv_event_t *event)
    {
        if (!event || lv_event_get_code(event) != LV_EVENT_KEY) return;
        const uint32_t key = lv_event_get_key(event);
        if (key == LV_KEY_ESC || key == LV_KEY_LEFT) {
            if (LeaveSelfPage) LeaveSelfPage();
        } else if (key == LV_KEY_ENTER || key == LV_KEY_RIGHT) {
            start();
        }
        lv_event_stop_processing(event);
    }

    cp0::ui::TitleRow title_{};
    cp0::ui::Panel actions_{};
    Row row_{};
    cp0::ui::SegBar progress_{};
    lv_obj_t *message_ = nullptr;
    lv_obj_t *footer_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    std::string message_text_;
    uint32_t message_color_ = cp0::theme::kIdle;
    std::string job_id_;
    int progress_value_ = 0;
};

} // namespace

std::unique_ptr<DComponens::LvglComponensBase> settings_updates_page_factory(lv_obj_t *parent,
                                                                            const NodeIter &,
                                                                            std::function<void()> back_callback)
{
    return std::make_unique<LvSettingUpdatesPage3>(parent, std::move(back_callback));
}

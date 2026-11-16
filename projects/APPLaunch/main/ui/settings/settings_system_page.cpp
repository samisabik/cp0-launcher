/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "settings_system_page.hpp"

#include "cp0_theme.hpp"
#include "hal_lvgl_bsp.h"
#include "settings_system_model.hpp"

#include <array>
#include <cstdint>
#include <list>
#include <string>
#include <utility>

namespace {

#define SETTINGS_SYSTEM_STRINGIFY_IMPL(value) #value
#define SETTINGS_SYSTEM_STRINGIFY(value) SETTINGS_SYSTEM_STRINGIFY_IMPL(value)

const char *launcher_version()
{
#ifdef LAUNCHER_VERSION
    return LAUNCHER_VERSION;
#elif defined(LAUNCHER_VERSION_RAW)
    return SETTINGS_SYSTEM_STRINGIFY(LAUNCHER_VERSION_RAW);
#else
    return "0.0.0-dev";
#endif
}

#undef SETTINGS_SYSTEM_STRINGIFY
#undef SETTINGS_SYSTEM_STRINGIFY_IMPL

constexpr uint32_t kPollMs = 500;

enum class Job { None, Check, Launcher, System };

struct ActionRow {
    Job job;
    const char *label;
};

constexpr std::array<ActionRow, 3> kActions = {{
    {Job::Check, "check for updates"},
    {Job::Launcher, "update launcher"},
    {Job::System, "refresh system packages"},
}};

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
        // A check is only a download of metadata; stop it.  An install keeps
        // running in its systemd unit and must not be interrupted.
        if (job_ != Job::None && job_ != Job::Launcher) {
            std::string ignored;
            osinfo_call({"UpdateJobCancel", job_id_}, ignored);
        }
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
        const int half = (cp0::theme::kScreenW - 2 * cp0::theme::kPadX - 4) / 2;
        installed_ = cp0::ui::readout(ComponensObj, cp0::theme::kPadX, 19, half, 42, "installed");
        available_ = cp0::ui::readout(ComponensObj, cp0::theme::kPadX + half + 4, 19, half, 42, "available");
        cp0::ui::set_readout(installed_, launcher_version());
        cp0::ui::set_readout(available_, "--", {}, cp0::theme::kIdle);

        actions_ = cp0::ui::panel(ComponensObj, cp0::theme::kPadX, 65, cp0::theme::kScreenW - 2 * cp0::theme::kPadX,
                                  52);
        for (std::size_t index = 0; index < kActions.size(); ++index) {
            const int y = 3 + static_cast<int>(index) * 16;
            rows_[index].band = cp0::ui::box(actions_.box, 2, y, cp0::theme::kScreenW - 2 * cp0::theme::kPadX - 4,
                                             15, cp0::theme::kSelect);
            rows_[index].text = cp0::ui::label(actions_.box, 8, y + 1, cp0::theme::kIdle);
            if (rows_[index].text) lv_label_set_text(rows_[index].text, kActions[index].label);
        }

        progress_ = cp0::ui::segbar(ComponensObj, cp0::theme::kPadX, 122, cp0::theme::kScreenW - 2 * cp0::theme::kPadX,
                                    4, cp0::theme::kBg);
        message_ = cp0::ui::label(ComponensObj, cp0::theme::kPadX, 132, cp0::theme::kIdle);
        if (message_) {
            lv_obj_set_width(message_, cp0::theme::kScreenW - 2 * cp0::theme::kPadX);
            lv_label_set_long_mode(message_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        }
        footer_ = cp0::ui::footer(ComponensObj);

        std::string last_state;
        if (osinfo_call({"UpdateLauncherState"}, last_state) == 0 && !last_state.empty())
            message_text_ = cp0::ui::lower(settings_system::launcher_state_label(last_state));
        else
            message_text_ = "ent runs the selected action";
        render();
    }

private:
    struct Row {
        lv_obj_t *band = nullptr;
        lv_obj_t *text = nullptr;
    };

    void render()
    {
        for (std::size_t index = 0; index < kActions.size(); ++index) {
            const bool selected = static_cast<int>(index) == selected_;
            if (rows_[index].band) {
                if (selected)
                    lv_obj_remove_flag(rows_[index].band, LV_OBJ_FLAG_HIDDEN);
                else
                    lv_obj_add_flag(rows_[index].band, LV_OBJ_FLAG_HIDDEN);
            }
            const bool dim = job_ != Job::None;
            cp0::ui::set_text(rows_[index].text, kActions[index].label,
                              dim ? cp0::theme::kDim : selected ? cp0::theme::kText : cp0::theme::kIdle);
        }
        cp0::ui::set_segbar(progress_, static_cast<float>(progress_value_) / 100.0f,
                            message_color_ == cp0::theme::kDanger ? cp0::theme::kDanger : cp0::theme::kAccent);
        cp0::ui::set_text(message_, message_text_.c_str(), message_color_);
        cp0::ui::set_text(title_.status, job_ == Job::None ? "" : "working", cp0::theme::kWarn);
        if (job_ != Job::None)
            cp0::ui::set_footer(footer_, {{"esc", "back"}});
        else
            cp0::ui::set_footer(footer_, {{"up/dn", "select"}, {"ent", "run"}, {"esc", "back"}});
    }

    void set_message(std::string text, uint32_t color)
    {
        message_text_ = std::move(text);
        message_color_ = color;
    }

    void start(Job job)
    {
        if (job_ != Job::None) return;
        const char *command = job == Job::Check      ? "UpdateLauncherCheckStart"
                              : job == Job::Launcher ? "UpdateLauncherStart"
                                                     : "AptUpdateStart";
        std::string id;
        if (osinfo_call({command}, id) != 0 || id.empty()) {
            set_message("could not start: " + cp0::ui::lower(id.empty() ? std::string("no job id") : id),
                        cp0::theme::kDanger);
            render();
            return;
        }
        job_ = job;
        job_id_ = id;
        progress_value_ = 0;
        set_message(job == Job::Check      ? "checking for a launcher update"
                    : job == Job::Launcher ? "refreshing packages, then installing"
                                           : "refreshing package lists",
                    cp0::theme::kIdle);
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
        if (job_ == Job::None) return;
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
            if (job_ == Job::Launcher && !info.stage.empty() && info.stage != "running") {
                const std::string stage = state.rfind("recovering:", 0) == 0 ? state : info.stage;
                std::string text = cp0::ui::lower(settings_system::update_job_label(
                    settings_system::UpdateAction::UpdateLauncher, 0, stage));
                if (info.progress >= 0) text += " " + std::to_string(info.progress) + "%";
                set_message(text, cp0::theme::kIdle);
            }
            render();
            return;
        }

        const Job finished = job_;
        finish();
        if (finished == Job::Check) {
            if (info.stage == "succeeded" && info.availability_known) {
                progress_value_ = 100;
                if (info.available) {
                    cp0::ui::set_readout(available_, info.version.empty() ? "new" : info.version, {},
                                         cp0::theme::kAccent);
                    set_message("update available. select update launcher", cp0::theme::kAccent);
                    selected_ = 1;
                } else {
                    cp0::ui::set_readout(available_, "current", {}, cp0::theme::kIdle);
                    set_message("launcher is up to date", cp0::theme::kIdle);
                }
            } else {
                set_message(failure_text("check failed", state), cp0::theme::kDanger);
            }
        } else {
            const auto action = finished == Job::Launcher ? settings_system::UpdateAction::UpdateLauncher
                                                          : settings_system::UpdateAction::CheckSystem;
            const bool succeeded = info.stage == "succeeded";
            if (succeeded) progress_value_ = 100;
            set_message(cp0::ui::lower(settings_system::update_job_label(action, succeeded ? 0 : -1, state)),
                        succeeded ? cp0::theme::kAccent : cp0::theme::kDanger);
        }
        render();
    }

    static std::string failure_text(const char *prefix, const std::string &state)
    {
        if (state == "cancelled") return "cancelled";
        std::string stage = state.rfind("failed:", 0) == 0 ? state.substr(7) : state;
        const std::size_t code = stage.find(':');
        if (code != std::string::npos) stage.resize(code);
        return std::string(prefix) + (stage.empty() ? "" : ": " + cp0::ui::lower(stage));
    }

    void finish()
    {
        job_ = Job::None;
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
        } else if (job_ == Job::None) {
            const int count = static_cast<int>(kActions.size());
            if (key == LV_KEY_UP) {
                selected_ = (selected_ + count - 1) % count;
                render();
            } else if (key == LV_KEY_DOWN) {
                selected_ = (selected_ + 1) % count;
                render();
            } else if (key == LV_KEY_ENTER || key == LV_KEY_RIGHT) {
                start(kActions[static_cast<std::size_t>(selected_)].job);
            }
        }
        lv_event_stop_processing(event);
    }

    cp0::ui::TitleRow title_{};
    cp0::ui::Readout installed_{};
    cp0::ui::Readout available_{};
    cp0::ui::Panel actions_{};
    std::array<Row, kActions.size()> rows_{};
    cp0::ui::SegBar progress_{};
    lv_obj_t *message_ = nullptr;
    lv_obj_t *footer_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    std::string message_text_;
    uint32_t message_color_ = cp0::theme::kIdle;
    std::string job_id_;
    Job job_ = Job::None;
    int selected_ = 0;
    int progress_value_ = 0;
};

} // namespace

std::unique_ptr<DComponens::LvglComponensBase> settings_updates_page_factory(lv_obj_t *parent,
                                                                            const NodeIter &,
                                                                            std::function<void()> back_callback)
{
    return std::make_unique<LvSettingUpdatesPage3>(parent, std::move(back_callback));
}

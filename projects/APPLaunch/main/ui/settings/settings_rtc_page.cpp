/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "settings_rtc_page.hpp"
#include "settings_fonts.hpp"
#include "cp0_theme.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include "cp0_lvgl_app.h"
#include "hal_lvgl_bsp.h"

namespace {

namespace setting {

enum class RtcField { YEAR, MONTH, DAY, HOUR, MINUTE, SECOND, COUNT };

class RtcStateModel {
public:
    using Values = std::array<int, static_cast<unsigned>(RtcField::COUNT)>;

    const Values &values() const noexcept { return values_; }
    bool dirty() const noexcept { return dirty_; }
    bool ntp_on() const noexcept { return ntp_on_; }
    bool ntp_available() const noexcept { return ntp_available_; }

    void set_ntp_status(int status) noexcept;
    void rollback_ntp(bool previous) noexcept { ntp_on_ = previous; }

    bool load_local_time(const std::tm &value) noexcept;

    int field_min(RtcField field) const noexcept;
    int field_max(RtcField field) const noexcept;
    int field_value(RtcField field) const noexcept;
    bool edit_field(RtcField field, int value) noexcept;

    void discard_edits() noexcept { dirty_ = false; }
    void finish_commit(bool succeeded) noexcept { dirty_ = !succeeded; }
    std::string timestamp() const;

    static int days_in_month(int year, int month) noexcept;
    static bool is_valid(const Values &values) noexcept;

private:
    static unsigned field_index(RtcField field) noexcept { return static_cast<unsigned>(field); }

    Values values_ = {2026, 1, 1, 0, 0, 0};
    bool ntp_on_ = true;
    // "Not observed yet", not "available": an unread status must map to
    // Unavailable instead of being reported as a state we never saw.
    bool ntp_available_ = false;
    bool dirty_ = false;
};

void RtcStateModel::set_ntp_status(int status) noexcept
{
    ntp_available_ = status == 0 || status == 1;
    if (ntp_available_) ntp_on_ = status == 1;
}

bool RtcStateModel::load_local_time(const std::tm &value) noexcept
{
    Values parsed = {
        value.tm_year + 1900, value.tm_mon + 1, value.tm_mday, value.tm_hour, value.tm_min, value.tm_sec,
    };
    if (!is_valid(parsed)) return false;
    values_ = parsed;
    dirty_ = false;
    return true;
}

int RtcStateModel::field_min(RtcField field) const noexcept
{
    static constexpr int minimums[] = {2000, 1, 1, 0, 0, 0};
    const unsigned index = field_index(field);
    return index < static_cast<unsigned>(RtcField::COUNT) ? minimums[index] : 0;
}

int RtcStateModel::field_max(RtcField field) const noexcept
{
    static constexpr int maximums[] = {2099, 12, 31, 23, 59, 59};
    const unsigned index = field_index(field);
    if (field == RtcField::DAY)
        return days_in_month(values_[field_index(RtcField::YEAR)], values_[field_index(RtcField::MONTH)]);
    return index < static_cast<unsigned>(RtcField::COUNT) ? maximums[index] : 0;
}

int RtcStateModel::field_value(RtcField field) const noexcept
{
    const unsigned index = field_index(field);
    return index < values_.size() ? values_[index] : 0;
}

bool RtcStateModel::edit_field(RtcField field, int value) noexcept
{
    const unsigned index = field_index(field);
    if (index >= values_.size() || value < field_min(field) || value > field_max(field)) return false;

    values_[index] = value;
    if (field == RtcField::YEAR || field == RtcField::MONTH) {
        const int maximum_day = field_max(RtcField::DAY);
        if (values_[field_index(RtcField::DAY)] > maximum_day) values_[field_index(RtcField::DAY)] = maximum_day;
    }
    dirty_ = true;
    return true;
}

std::string RtcStateModel::timestamp() const
{
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d", values_[0], values_[1], values_[2],
                  values_[3], values_[4], values_[5]);
    return buffer;
}

int RtcStateModel::days_in_month(int year, int month) noexcept
{
    static constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month != 2) return days[month - 1];
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return leap ? 29 : 28;
}

bool RtcStateModel::is_valid(const Values &values) noexcept
{
    if (values[0] < 2000 || values[0] > 2099 || values[1] < 1 || values[1] > 12) return false;
    return values[2] >= 1 && values[2] <= days_in_month(values[0], values[1]) && values[3] >= 0 &&
           values[3] <= 23 && values[4] >= 0 && values[4] <= 59 && values[5] >= 0 && values[5] <= 59;
}

#define SETTINGS_RTC_STRINGIFY_IMPL(value) #value
#define SETTINGS_RTC_STRINGIFY(value) SETTINGS_RTC_STRINGIFY_IMPL(value)

std::string_view factory_build_date() noexcept
{
#ifdef LAUNCHER_BUILD_DATE_RAW
    return SETTINGS_RTC_STRINGIFY(LAUNCHER_BUILD_DATE_RAW);
#else
    return {};
#endif
}

#undef SETTINGS_RTC_STRINGIFY
#undef SETTINGS_RTC_STRINGIFY_IMPL

bool timestamp_before_factory_date(std::string_view timestamp) noexcept
{
    const std::string_view build_date = factory_build_date();
    if (timestamp.size() < 10 || build_date.size() != 10) return false;
    if (build_date[4] != '-' || build_date[7] != '-') return false;
    static constexpr std::size_t kDateDigitIndexes[] = {0, 1, 2, 3, 5, 6, 8, 9};
    for (const std::size_t index : kDateDigitIndexes) {
        if (build_date[index] < '0' || build_date[index] > '9') return false;
    }
    return timestamp.substr(0, 10) < build_date;
}

} // namespace setting

namespace settings_rtc {

using RtcField = setting::RtcField;
using RtcStateModel = setting::RtcStateModel;

enum class PrivilegedResultKind { SUCCESS, AUTH_FAILED, CANCELLED, TIMED_OUT, EXEC_FAILED };

enum class ApiError : int {
    InvalidArgument = -22,
    Invocation = -5,
};

constexpr int api_error_code(ApiError error) noexcept
{
    return static_cast<int>(error);
}

struct NtpReadResult {
    int status = -1;
    bool available = false;
    bool enabled = false;
};

struct PrivilegedResult {
    int result_code = 1;
    int exit_code = 0;
    PrivilegedResultKind kind = PrivilegedResultKind::EXEC_FAILED;

    bool succeeded() const noexcept
    {
        return kind == PrivilegedResultKind::SUCCESS && result_code == 0 && exit_code == 0;
    }
};

using NtpReadCallback = std::function<void(NtpReadResult)>;
using PrivilegedCallback = std::function<void(PrivilegedResult)>;
using RequestStartedCallback = std::function<void(int, std::uint64_t)>;

enum class WorkflowOperation { IDLE, NTP_SET, TIME_SET };

enum class CommitEligibility { ALLOWED, NO_EDITS, NTP_ENABLED, IN_FLIGHT };

class RtcWorkflowModel {
public:
    RtcStateModel &state() noexcept { return state_; }
    const RtcStateModel &state() const noexcept { return state_; }

    bool pending() const noexcept { return operation_ != WorkflowOperation::IDLE; }
    bool ntp_pending() const noexcept { return operation_ == WorkflowOperation::NTP_SET; }
    bool time_pending() const noexcept { return operation_ == WorkflowOperation::TIME_SET; }

    void set_ntp_status(int status) noexcept { state_.set_ntp_status(status); }
    void discard_edits() noexcept { state_.discard_edits(); }

    CommitEligibility commit_eligibility() const noexcept
    {
        if (pending()) return CommitEligibility::IN_FLIGHT;
        if (state_.ntp_on()) return CommitEligibility::NTP_ENABLED;
        if (!state_.dirty()) return CommitEligibility::NO_EDITS;
        return CommitEligibility::ALLOWED;
    }

    bool begin_time_commit() noexcept
    {
        if (commit_eligibility() != CommitEligibility::ALLOWED) return false;
        operation_ = WorkflowOperation::TIME_SET;
        return true;
    }

    void finish_time_commit(bool succeeded) noexcept
    {
        if (!time_pending()) return;
        state_.finish_commit(succeeded);
        operation_ = WorkflowOperation::IDLE;
    }

    void cancel_time_commit() noexcept
    {
        if (time_pending()) operation_ = WorkflowOperation::IDLE;
    }

    bool begin_ntp_toggle(bool desired) noexcept
    {
        if (pending() || state_.dirty() || !state_.ntp_available()) return false;
        ntp_previous_ = state_.ntp_on();
        ntp_desired_ = desired;
        operation_ = WorkflowOperation::NTP_SET;
        return true;
    }

    void finish_ntp_toggle(bool succeeded, int actual_status = -1) noexcept
    {
        if (!ntp_pending()) return;
        if (succeeded) {
            const int status = actual_status == 0 || actual_status == 1 ? actual_status : (ntp_desired_ ? 1 : 0);
            state_.set_ntp_status(status);
        } else {
            state_.rollback_ntp(ntp_previous_);
        }
        operation_ = WorkflowOperation::IDLE;
    }

    void cancel_ntp_toggle() noexcept
    {
        if (!ntp_pending()) return;
        state_.rollback_ntp(ntp_previous_);
        operation_ = WorkflowOperation::IDLE;
    }

private:
    RtcStateModel state_;
    WorkflowOperation operation_ = WorkflowOperation::IDLE;
    bool ntp_previous_ = true;
    bool ntp_desired_ = true;
};

RtcWorkflowModel &session() noexcept
{
    static RtcWorkflowModel instance;
    return instance;
}

PrivilegedResultKind classify_privileged_result(int result, int exit_code) noexcept
{
    if (result == 0 && exit_code != 0) return PrivilegedResultKind::EXEC_FAILED;
    switch (result) {
    case 0: return PrivilegedResultKind::SUCCESS;
    case 1: return PrivilegedResultKind::AUTH_FAILED;
    case 3: return PrivilegedResultKind::CANCELLED;
    case 4: return PrivilegedResultKind::TIMED_OUT;
    default: return PrivilegedResultKind::EXEC_FAILED;
    }
}

template <typename Callback, typename Value>
void invoke_noexcept(const Callback &callback, Value value) noexcept
{
    if (!callback) return;
    try {
        callback(std::move(value));
    } catch (...) {
    }
}

struct NtpAdapterState {
    std::mutex mutex;
    // Start "unknown" rather than "on": a green tick before the first
    // successful read would claim an NTP state this process never observed.
    bool available = false;
    bool enabled = false;
    bool pending = false;
    bool initialized = false;
};

NtpAdapterState &ntp_adapter_state()
{
    static NtpAdapterState state;
    return state;
}

int read_ntp_async(NtpReadCallback callback)
{
    if (!callback) return api_error_code(ApiError::InvalidArgument);
    cp0_signal_timedate_api({"NtpGet"}, [callback = std::move(callback)](int code, std::string) {
        NtpReadResult result;
        result.status = code;
        result.available = code == 0 || code == 1;
        result.enabled = code == 1;
        invoke_noexcept(callback, std::move(result));
    });
    return 0;
}

// The status icon and the "set manually" gate read the cached value as soon as
// this returns, so the backend must deliver its callback inline.  It does today
// (osinfo dispatch invokes synchronously); if that ever changes, both readers
// silently go stale.
void refresh_ntp_cache(bool force = false)
{
    auto &state = ntp_adapter_state();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.pending || (!force && state.initialized)) return;
        state.pending = true;
    }

    const int result = read_ntp_async([](NtpReadResult value) {
        session().set_ntp_status(value.status);
        auto &state = ntp_adapter_state();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.pending = false;
        state.initialized = true;
        state.available = value.available;
        if (value.available) state.enabled = value.enabled;
    });
    if (result != 0) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.pending = false;
        state.initialized = true;
        state.available = false;
    }
}

int submit_privileged(std::list<std::string> arguments, PrivilegedCallback callback, RequestStartedCallback started)
{
    if (!callback) return api_error_code(ApiError::InvalidArgument);
    try {
        cp0_signal_system_admin_async(
            std::move(arguments), 60000, 30000,
            [callback = std::move(callback)](int result_code, int exit_code) mutable {
                PrivilegedResult result;
                result.result_code = result_code;
                result.exit_code = exit_code;
                result.kind = classify_privileged_result(result_code, exit_code);
                invoke_noexcept(callback, std::move(result));
            },
            [started = std::move(started)](int start_code, std::uint64_t request_id) mutable {
                if (!started) return;
                try {
                    started(start_code, request_id);
                } catch (...) {
                }
            });
    } catch (...) {
        return api_error_code(ApiError::Invocation);
    }
    return 0;
}

// Writing the clock needs privileges.  The timedated D-Bus API answers an
// unprivileged session with InteractiveAuthorizationRequired, so the write
// goes through the sudo coordinator, which also hands back a cancellable
// request id and writes the hardware RTC via `hwclock -w`.
int set_ntp_async(bool enabled, PrivilegedCallback callback)
{
    return submit_privileged({"NtpSet", enabled ? "1" : "0"}, std::move(callback), {});
}

int set_time_async(std::string timestamp, PrivilegedCallback callback, RequestStartedCallback started)
{
    return submit_privileged({"TimeSet", std::move(timestamp)}, std::move(callback), std::move(started));
}

void cancel_request(std::uint64_t request_id)
{
    if (request_id == 0) return;
    try {
        cp0_signal_sudo_cancel(request_id, [](int) {});
    } catch (...) {
    }
}

const char *write_error_message(PrivilegedResultKind result) noexcept
{
    switch (result) {
    case PrivilegedResultKind::AUTH_FAILED: return "authentication failed";
    case PrivilegedResultKind::CANCELLED: return "write cancelled";
    case PrivilegedResultKind::TIMED_OUT: return "write timed out";
    case PrivilegedResultKind::EXEC_FAILED:
    case PrivilegedResultKind::SUCCESS: break;
    }
    return "write failed";
}

} // namespace settings_rtc

// yyyy-mm-dd hh:mm: LEFT/RIGHT picks a field, UP/DOWN changes it with wrap
// around, ENTER writes, ESC cancels.  Seconds are written as 00.
class LvSettingTimeEditorPage3 : public DComponens::LvglComponensBase {
public:
    LvSettingTimeEditorPage3(lv_obj_t *parent, std::function<void()> back_callback)
    {
        LeaveSelfPage = std::move(back_callback);
        load_now();
        create_ui(parent);
    }

    ~LvSettingTimeEditorPage3() override
    {
        cancel_request();
        settings_rtc::session().discard_edits();
        cancel_async_tasks();
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
        ensure_async_dispatch();
        ComponensObj = cp0::ui::box(parent, 0, 0, cp0::theme::kScreenW, cp0::theme::kScreenH, cp0::theme::kBg);
        if (!ComponensObj) return;
        DComponens::lvgl_bind_event(ComponensObj, LV_EVENT_KEY, nullptr,
                                    [this](lv_event_t *event) { handle_key_event(event); });

        cp0::ui::dot_grid(ComponensObj, 0, cp0::theme::kBodyTop, cp0::theme::kScreenW, cp0::theme::kBodyH);
        title_ = cp0::ui::title_row(ComponensObj, "settings/time/set manually");

        static constexpr const char *kCaptions[kFieldCount] = {"year", "month", "day", "hour", "min"};
        static constexpr int kWidths[kFieldCount] = {70, 44, 44, 44, 44};
        static constexpr const char *kSeparators[kFieldCount - 1] = {"-", "-", "", ":"};
        int x = 21;
        for (int index = 0; index < kFieldCount; ++index) {
            fields_[index] = cp0::ui::readout(ComponensObj, x, 42, kWidths[index], 44, kCaptions[index]);
            x += kWidths[index];
            if (index + 1 < kFieldCount) {
                lv_obj_t *separator = cp0::ui::label(ComponensObj, x + 1, 64, cp0::theme::kDim);
                if (separator) lv_label_set_text(separator, kSeparators[index]);
                x += 8;
            }
        }
        status_ = cp0::ui::label(ComponensObj, cp0::theme::kPadX + 14, 100, cp0::theme::kIdle);
        if (status_) {
            lv_obj_set_width(status_, cp0::theme::kScreenW - 2 * (cp0::theme::kPadX + 14));
            lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
        }
        footer_ = cp0::ui::footer(ComponensObj);
        render();
    }

private:
    static constexpr int kFieldCount = 5;
    static constexpr settings_rtc::RtcField kFields[kFieldCount] = {
        settings_rtc::RtcField::YEAR, settings_rtc::RtcField::MONTH, settings_rtc::RtcField::DAY,
        settings_rtc::RtcField::HOUR, settings_rtc::RtcField::MINUTE};

    struct RequestState {
        std::atomic<std::uint64_t> request_id{0};
        std::atomic_bool cancelled{false};
    };

    static void load_now()
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        if (localtime_r(&now, &local)) settings_rtc::session().state().load_local_time(local);
    }

    void render()
    {
        const auto &state = settings_rtc::session().state();
        for (int index = 0; index < kFieldCount; ++index) {
            char text[8];
            std::snprintf(text, sizeof(text), index == 0 ? "%04d" : "%02d", state.field_value(kFields[index]));
            const bool focus = index == focus_;
            cp0::ui::set_readout(fields_[index], text, {}, focus ? cp0::theme::kText : cp0::theme::kIdle);
            cp0::ui::set_panel_focus(fields_[index].panel, focus);
        }
        if (writing_) {
            cp0::ui::set_text(title_.status, "writing", cp0::theme::kWarn);
            cp0::ui::set_footer_message(footer_, "writing clock and rtc...", cp0::theme::kIdle);
        } else {
            cp0::ui::set_text(title_.status, "", cp0::theme::kIdle);
            cp0::ui::set_footer(footer_, {{"l/r", "field"}, {"up/dn", "change"}, {"ent", "write"}, {"esc", "cancel"}});
        }
    }

    void set_status(const char *text, uint32_t color)
    {
        cp0::ui::set_text(status_, text, color);
    }

    void step(int delta)
    {
        auto &state = settings_rtc::session().state();
        const settings_rtc::RtcField field = kFields[focus_];
        const int minimum = state.field_min(field);
        const int maximum = state.field_max(field);
        int value = state.field_value(field) + delta;
        if (value > maximum) value = minimum;
        if (value < minimum) value = maximum;
        state.edit_field(field, value);
        confirm_old_date_ = false;
        set_status("", cp0::theme::kIdle);
        render();
    }

    void write()
    {
        if (writing_) return;
        auto &workflow = settings_rtc::session();
        if (const ActivationBlock *block = settings_rtc_manual_edit_block()) {
            set_status(block->title, cp0::theme::kDanger);
            return;
        }
        auto &state = workflow.state();
        state.edit_field(settings_rtc::RtcField::SECOND, 0);
        const std::string timestamp = state.timestamp();
        if (!confirm_old_date_ && setting::timestamp_before_factory_date(timestamp)) {
            confirm_old_date_ = true;
            set_status("this date is before the launcher build. ent again to write it.", cp0::theme::kWarn);
            return;
        }
        if (!workflow.begin_time_commit()) {
            set_status("another clock change is in progress", cp0::theme::kDanger);
            return;
        }

        auto request = std::make_shared<RequestState>();
        request_ = request;
        writing_ = true;
        set_status("", cp0::theme::kIdle);
        render();
        const auto token = async_token();
        const int start = settings_rtc::set_time_async(
            timestamp,
            [this, token, request](settings_rtc::PrivilegedResult result) {
                SettingsAsync::Dispatch::enqueue_from_callback(token, [this, request, result] {
                    if (request_ != request) return;
                    finish_write(result);
                });
            },
            [request](int start_code, std::uint64_t request_id) {
                if (start_code != 0) return;
                request->request_id.store(request_id);
                if (request->cancelled.load()) settings_rtc::cancel_request(request->request_id.exchange(0));
            });
        if (start != 0) {
            settings_rtc::PrivilegedResult failed;
            finish_write(failed);
        }
    }

    void finish_write(const settings_rtc::PrivilegedResult &result)
    {
        request_.reset();
        writing_ = false;
        settings_rtc::session().finish_time_commit(result.succeeded());
        if (result.succeeded()) {
            if (LeaveSelfPage) LeaveSelfPage();
            return;
        }
        set_status(settings_rtc::write_error_message(result.kind), cp0::theme::kDanger);
        render();
    }

    void cancel_request()
    {
        if (request_) {
            request_->cancelled.store(true);
            settings_rtc::cancel_request(request_->request_id.exchange(0));
            request_.reset();
        }
        settings_rtc::session().cancel_time_commit();
        writing_ = false;
    }

    void handle_key_event(lv_event_t *event)
    {
        if (!event || lv_event_get_code(event) != LV_EVENT_KEY) return;
        const uint32_t key = lv_event_get_key(event);
        if (key == LV_KEY_ESC) {
            cancel_request();
            if (LeaveSelfPage) LeaveSelfPage();
        } else if (!writing_) {
            if (key == LV_KEY_LEFT && focus_ > 0) {
                --focus_;
                render();
            } else if (key == LV_KEY_RIGHT && focus_ + 1 < kFieldCount) {
                ++focus_;
                render();
            } else if (key == LV_KEY_UP) {
                step(1);
            } else if (key == LV_KEY_DOWN) {
                step(-1);
            } else if (key == LV_KEY_ENTER) {
                write();
            }
        }
        lv_event_stop_processing(event);
    }

    std::array<cp0::ui::Readout, kFieldCount> fields_{};
    cp0::ui::TitleRow title_{};
    lv_obj_t *status_ = nullptr;
    lv_obj_t *footer_ = nullptr;
    int focus_ = 0;
    bool writing_ = false;
    bool confirm_old_date_ = false;
    std::shared_ptr<RequestState> request_;
};

} // namespace

void settings_rtc_ntp_api(int command, void *data) noexcept
{
    auto &state = settings_rtc::ntp_adapter_state();
    if (command == SettingApiReadFlag || command == SettingApiReadFlagTimeStart) {
        if (command == SettingApiReadFlagTimeStart) settings_rtc::refresh_ntp_cache();

        bool enabled = false;
        bool pending = false;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            enabled = state.available && state.enabled;
            pending = state.pending;
        }
        if (!data) return;

        if (command == SettingApiReadFlag) {
            *static_cast<bool *>(data) = enabled;
        } else {
            auto *result = static_cast<SettingApiReadFlagTimeStartData *>(data);
            std::get<0>(*result) = enabled;
            if (std::get<1>(*result)) std::get<1>(*result)->store(pending, std::memory_order_release);
        }
        return;
    }
    if (command != SettingApiActivate) return;

    auto &session = settings_rtc::session();
    bool desired = false;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.pending || !state.available) return;
        desired = !state.enabled;
        state.pending = true;
    }
    // Switching the NTP mode supersedes any manual edit that was not written.
    session.discard_edits();
    if (!session.begin_ntp_toggle(desired)) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.pending = false;
        return;
    }

    const int result = settings_rtc::set_ntp_async(desired, [desired](settings_rtc::PrivilegedResult value) {
        settings_rtc::session().finish_ntp_toggle(value.succeeded(), desired ? 1 : 0);
        auto &state = settings_rtc::ntp_adapter_state();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.pending = false;
        if (value.succeeded()) {
            state.available = true;
            state.enabled = desired;
        }
        state.initialized = true;
    });
    if (result != 0) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.pending = false;
        session.cancel_ntp_toggle();
    }
}

const ActivationBlock *settings_rtc_manual_edit_block() noexcept
{
    static constexpr ActivationBlock kInFlight{"rtc operation in progress", "wait for it to finish, then try again."};
    static constexpr ActivationBlock kUnavailable{"network time status unavailable",
                                                  "cannot change the clock right now."};
    static constexpr ActivationBlock kNetworkTimeOn{"network time is on",
                                                    "turn off network time to set the clock manually."};

    auto &workflow = settings_rtc::session();
    if (workflow.pending()) return &kInFlight;

    const auto &state = workflow.state();
    if (!state.ntp_available()) return &kUnavailable;
    if (state.ntp_on()) return &kNetworkTimeOn;
    return nullptr;
}

void settings_rtc_refresh_ntp() noexcept
{
    // force: the caller needs a value observed now, not the first one this
    // process happened to see.
    settings_rtc::refresh_ntp_cache(true);
}

std::unique_ptr<DComponens::LvglComponensBase> settings_rtc_editor_page_factory(lv_obj_t *parent,
                                                                               const NodeIter &,
                                                                               std::function<void()> back_callback)
{
    return std::make_unique<LvSettingTimeEditorPage3>(parent, std::move(back_callback));
}

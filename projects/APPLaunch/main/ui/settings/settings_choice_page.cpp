/*
 * SPDX-License-Identifier: MIT
 */

#include "settings_choice_page.hpp"

#include "cp0_theme.hpp"
#include "settings_fonts.hpp"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <utility>

std::string settings_run_admin(std::list<std::string> arguments, int timeout_ms)
{
    struct State {
        std::mutex mutex;
        std::condition_variable condition;
        bool done = false;
        int result = -1;
        int exit_code = -1;
    };
    auto state = std::make_shared<State>();
    auto finish = [state](int result, int exit_code) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->done) return;
        state->done = true;
        state->result = result;
        state->exit_code = exit_code;
        state->condition.notify_all();
    };
    try {
        cp0_signal_system_admin_async(
            std::move(arguments), timeout_ms, timeout_ms, [finish](int result, int exit_code) { finish(result, exit_code); },
            [finish](int start_code, std::uint64_t) {
                if (start_code != 0) finish(start_code, -1);
            });
    } catch (...) {
        return "cannot run helper";
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(lock, std::chrono::milliseconds(timeout_ms + 5000),
                                   [state] { return state->done; }))
        return "timed out";
    if (state->result != 0) return "not permitted";
    if (state->exit_code != 0) return "failed (" + std::to_string(state->exit_code) + ")";
    return {};
}

LvSettingChoicePage3::LvSettingChoicePage3(lv_obj_t *parent, const NodeIter &parent_node,
                                           std::function<void()> back_callback, Reader reader, Writer writer,
                                           Applied applied, Preview preview)
    : LvSettingValuePage3Base(parent_node, {}), reader_(std::move(reader)), writer_(std::move(writer)),
      applied_(std::move(applied)), preview_(std::move(preview)), back_callback_(std::move(back_callback))
{
    LeaveSelfPage = [this] { request_back(); };
    initialize(parent);
    if (ComponensObj) {
        status_label_ = lv_label_create(ComponensObj);
        if (status_label_) {
            lv_obj_set_width(status_label_, 200);
            lv_label_set_long_mode(status_label_, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_font(status_label_, settings_fonts::mono(kCp0ChromeFontSize), LV_PART_MAIN);
            lv_obj_set_pos(status_label_, 4, metric(LayoutMetric::ScreenH) - 14);
            lv_label_set_text(status_label_, "");
        }
    }
    begin_load();
}

LvSettingChoicePage3::~LvSettingChoicePage3()
{
    page_alive_ = false;
    ++generation_;
    LeaveSelfPage = nullptr;
    cancel_async_tasks();
    back_callback_ = nullptr;
    status_label_ = nullptr;
}

int LvSettingChoicePage3::initial_selection() const
{
    return saved_index_ >= 0 ? saved_index_ : 0;
}

void LvSettingChoicePage3::request_back()
{
    if (back_requested_) return;
    back_requested_ = true;
    pending_ = false;
    ++generation_;
    cancel_async_tasks();
    // Leaving without confirming puts the saved option back on screen.
    if (preview_ && saved_index_ >= 0) preview_(saved_index_);
    if (back_callback_) back_callback_();
}

void LvSettingChoicePage3::selection_moved()
{
    if (preview_ && !pending_) preview_(selected_index);
}

void LvSettingChoicePage3::set_status(const std::string &text, bool error)
{
    if (!status_label_) return;
    lv_label_set_text(status_label_, cp0::ui::lower(text).c_str());
    lv_obj_set_style_text_color(status_label_, lv_color_hex(error ? cp0::theme::kDanger : cp0::theme::kDim), LV_PART_MAIN);
}

bool LvSettingChoicePage3::request_is_current(std::uint64_t generation) const
{
    return page_alive_ && generation_ == generation;
}

SettingApiResult LvSettingChoicePage3::activate_selected()
{
    if (pending_) return SettingApiResult::Pending;
    if (selected_index == saved_index_) {
        request_back();
        return SettingApiResult::Success;
    }
    begin_write();
    return SettingApiResult::Pending;
}

void LvSettingChoicePage3::begin_load()
{
    if (!reader_) return;
    pending_ = true;
    const std::uint64_t generation = ++generation_;
    set_status("reading", false);
    DComponens::LvglComponensBase::AsyncTaskCallbacks<int> callbacks;
    Reader reader = reader_;
    callbacks.execute = [reader]() { return reader(); };
    callbacks.on_complete = [this, generation](DComponens::LvglComponensBase::AsyncTaskContext &, const int &index) {
        if (!request_is_current(generation)) return;
        pending_ = false;
        saved_index_ = index;
        if (index >= 0) select(index);
        set_status(index >= 0 ? "" : "current value unknown", index < 0);
        if (ComponensObj && lv_obj_get_group(ComponensObj)) lv_group_focus_obj(ComponensObj);
    };
    auto fail = [this, generation](DComponens::LvglComponensBase::AsyncTaskContext &) {
        if (!request_is_current(generation)) return;
        pending_ = false;
        set_status("read failed", true);
    };
    callbacks.on_exception = [fail](DComponens::LvglComponensBase::AsyncTaskContext &context, std::exception_ptr) {
        fail(context);
    };
    callbacks.on_timeout = fail;
    callbacks.on_schedule_failed = fail;
    DComponens::LvglComponensBase::AsyncTaskOptions options;
    options.timeout = std::chrono::milliseconds(10000);
    if (!run_async_task(std::move(callbacks), options) && request_is_current(generation)) {
        pending_ = false;
        set_status("read failed", true);
    }
}

void LvSettingChoicePage3::begin_write()
{
    pending_ = true;
    const std::uint64_t generation = ++generation_;
    const int target = selected_index;
    set_status("applying", false);
    DComponens::LvglComponensBase::AsyncTaskCallbacks<std::string> callbacks;
    Writer writer = writer_;
    callbacks.execute = [writer, target]() { return writer ? writer(target) : std::string("unavailable"); };
    callbacks.on_complete = [this, generation, target](DComponens::LvglComponensBase::AsyncTaskContext &,
                                                       const std::string &error) {
        if (!request_is_current(generation)) return;
        pending_ = false;
        if (error.empty()) {
            saved_index_ = target;
            parent_node()->selected_index = target;
            if (applied_) applied_(target);
            request_back();
            return;
        }
        if (saved_index_ >= 0) select(saved_index_);
        set_status(error, true);
    };
    auto fail = [this, generation](DComponens::LvglComponensBase::AsyncTaskContext &) {
        if (!request_is_current(generation)) return;
        pending_ = false;
        set_status("failed", true);
    };
    callbacks.on_exception = [fail](DComponens::LvglComponensBase::AsyncTaskContext &context, std::exception_ptr) {
        fail(context);
    };
    callbacks.on_timeout = fail;
    callbacks.on_schedule_failed = fail;
    DComponens::LvglComponensBase::AsyncTaskOptions options;
    options.timeout = std::chrono::milliseconds(40000);
    if (!run_async_task(std::move(callbacks), options) && request_is_current(generation)) {
        pending_ = false;
        set_status("failed", true);
    }
}

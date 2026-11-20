/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "settings_adapter.hpp"

#include "cp0_lvgl_app.h"
#include "hal_lvgl_bsp.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <list>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>

namespace settings_t12b {
namespace {

void mark_operation_started(void *data)
{
    if (!data) return;
    auto *result = static_cast<SettingApiReadFlagTimeStartData *>(data);
    if (std::get<1>(*result))
        std::get<1>(*result)->store(true, std::memory_order_release);
}

} // namespace

bool populate_launcher_children(Tree &tree, const NodeIter &parent)
{
    tree.erase_children(parent);
    std::size_t count = 0;
    const AppDescriptor *entries = launcher_app_registry_entries(&count);
    if (count > 0 && !entries) return false;

    for (std::size_t index = 0; index < count; ++index) {
        const AppDescriptor &descriptor = entries[index];
        if (!descriptor.configurable || !descriptor.label || !descriptor.label[0] ||
            !descriptor.config_key || !descriptor.config_key[0])
            continue;
        tree.append_child(
            parent,
            SettingEntry{descriptor.label, make_launcher_toggle_api(descriptor), true});
    }
    return true;
}

namespace {

struct LauncherToggleState
{
    mutable std::mutex mutex;
    bool value = false;
    bool initialized = false;
    bool pending = false;
};

} // namespace

SettingApiCallBackFunc make_launcher_toggle_api(const AppDescriptor &descriptor)
{
    const std::string config_key = descriptor.config_key ? descriptor.config_key : "";
    const bool configurable = descriptor.configurable;
    const bool always_on = descriptor.always_on;
    auto state = std::make_shared<LauncherToggleState>();
    return [state, config_key, configurable, always_on](int command, void *data) {
        const AppDescriptor current_descriptor{nullptr,
                                               config_key.c_str(),
                                               configurable,
                                               always_on};
        if (command == SettingApiReadFlag && data) {
            bool registry_value = false;
            bool registry_read = false;
            try {
                registry_value = launcher_app_registry_is_enabled(current_descriptor);
                registry_read = true;
            } catch (...) {
            }

            bool value = registry_value;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->pending) {
                    value = state->value;
                } else if (registry_read) {
                    state->value = registry_value;
                } else {
                    value = state->value;
                }
                state->initialized = true;
            }
            *static_cast<bool *>(data) = value;
            return;
        }

        if (command == SettingApiReadFlagTimeStart && data) {
            mark_operation_started(data);
            auto *result = static_cast<SettingApiReadFlagTimeStartData *>(data);
            bool registry_value = false;
            bool registry_read = false;
            try {
                registry_value = launcher_app_registry_is_enabled(current_descriptor);
                registry_read = true;
            } catch (...) {
            }

            bool value = registry_value;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->pending) {
                    value = state->value;
                } else if (registry_read) {
                    state->value = registry_value;
                } else {
                    value = state->value;
                }
                state->initialized = true;
            }
            std::get<0>(*result) = value;
            return;
        }

        if (command != SettingApiActivate) return;

        bool previous = false;
        bool registry_read = false;
        try {
            previous = launcher_app_registry_is_enabled(current_descriptor);
            registry_read = true;
        } catch (...) {
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->pending) return;
            if (!registry_read) previous = state->initialized ? state->value : false;
            state->pending = true;
            state->value = !previous;
            state->initialized = true;
        }

        bool succeeded = false;
        try {
            succeeded = launcher_app_registry_set_enabled(current_descriptor, !previous);
        } catch (...) {
            succeeded = false;
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->pending = false;
            state->value = succeeded ? !previous : previous;
        }
        if (succeeded) launcher_app_registry_notify_changed();
    };
}

} // namespace settings_t12b

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "hal_lvgl_bsp.h"
#include "../cp0_callback_contract.hpp"
#include "../cp0_signal_registration.hpp"
#include "../cp0_update_job.hpp"

#include <cerrno>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <string>
#include <utility>

namespace {

class SdlOsInfoSystem {
public:
    using callback_t = std::function<void(int, std::string)>;
    using arg_t = std::list<std::string>;

    void api_call(arg_t arg, callback_t callback)
    {
        const std::string command = arg.empty() ? std::string() : arg.front();
        if (command == "SystemUpgradeStart") {
            const std::string id = jobs_.start([] {
                return cp0::update::Result{-ENOTSUP, "unsupported"};
            });
            cp0::callback::invoke(callback, 0, id);
            return;
        }
        if (command == "UpdateJobStatus") {
            const auto id = arg.size() > 1 ? *std::next(arg.begin()) : std::string();
            std::string status;
            cp0::callback::invoke(callback, jobs_.status(id, status) ? 0 : -1, status);
            return;
        }
        if (command == "NtpGet") {
            cp0::callback::invoke(callback, 0, {});
            return;
        }
        cp0::callback::invoke(callback, -1, "unknown osinfo api command");
    }

private:
    cp0::update::Jobs jobs_;
};

} // namespace

extern "C" void init_osinfo(void)
{
    static cp0::SignalRegistration<decltype(cp0_signal_osinfo_api)> registration;
    static cp0::SignalRegistration<decltype(cp0_signal_timedate_api)> timedate_registration;
    auto osinfo = std::make_shared<SdlOsInfoSystem>();
    registration.replace(cp0_signal_osinfo_api, [osinfo](std::list<std::string> arg, std::function<void(int, std::string)> callback) {
        osinfo->api_call(std::move(arg), std::move(callback));
    });
    timedate_registration.replace(cp0_signal_timedate_api, [osinfo](std::list<std::string> arg, std::function<void(int, std::string)> callback) {
        osinfo->api_call(std::move(arg), std::move(callback));
    });
}

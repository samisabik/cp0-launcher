/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "cp0_lvgl_app.h"
#include "hal_lvgl_bsp.h"
#include "../cp0_callback_contract.hpp"
#include "../cp0_signal_registration.hpp"
#include "../cp0_update_job.hpp"
#include "../cp0_system_upgrade.hpp"
#include "cp0_process_commands.hpp"
#include "cp0_timedate_client.hpp"

#include <atomic>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <list>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

class OsInfoSystem
{
public:
    using callback_t = std::function<void(int, std::string)>;
    using arg_t = std::list<std::string>;

    void api_call(arg_t arg, callback_t callback)
    {
        const std::string command = arg.empty() ? std::string() : arg.front();
        if (command == "SystemUpgradeStart") {
            // One upgrade at a time: a second start returns the running job so
            // a reopened settings page can follow it again.
            std::lock_guard<std::mutex> lock(upgrade_mutex_);
            if (!upgrade_running_->load()) {
                upgrade_running_->store(true);
                const std::string log = system_upgrade_log_path();
                auto running = upgrade_running_;
                upgrade_id_ = jobs_.start(
                    [log, running] {
                        const cp0::update::Result result = system_upgrade(log);
                        running->store(false);
                        return result;
                    },
                    [log] { return cp0::system_upgrade::progress(read_file(log)); });
            }
            cp0::callback::invoke(callback, 0, upgrade_id_);
            return;
        }
        if (command == "UpdateJobStatus") {
            const auto id = arg.size() > 1 ? *std::next(arg.begin()) : std::string();
            std::string status;
            cp0::callback::invoke(callback, jobs_.status(id, status) ? 0 : -1, status);
            return;
        }
        if (command == "NtpGet") {
            cp0::callback::invoke(callback, ntp_get(), {});
            return;
        }
        cp0::callback::invoke(callback, -1, "unknown osinfo api command");
    }

private:
    cp0::update::Jobs jobs_;
    std::mutex upgrade_mutex_;
    std::shared_ptr<std::atomic<bool>> upgrade_running_ = std::make_shared<std::atomic<bool>>(false);
    std::string upgrade_id_;

    static std::string system_upgrade_log_path()
    {
        const char *runtime = std::getenv("XDG_RUNTIME_DIR");
        return std::string(runtime && runtime[0] ? runtime : "/tmp") + "/cp0-system-upgrade.log";
    }

    static std::string read_file(const std::string &path)
    {
        std::ifstream input(path);
        std::ostringstream text;
        text << input.rdbuf();
        return text.str();
    }

    // apt-get update, then a non-interactive apt-get upgrade that keeps local
    // config files. Held packages (the launcher itself) stay as they are.
    // Neither step is cancellable: interrupting dpkg leaves it half-configured.
    static cp0::update::Result system_upgrade(const std::string &log)
    {
        const std::string apt =
            "sudo -n env DEBIAN_FRONTEND=noninteractive apt-get -q -o APT::Status-Fd=1 ";
        const std::string quoted_log = "'" + log + "'";
        std::string output;
        int code = cp0_process_commands::capture_argv_with_timeout(
            {"sh", "-c", std::string("echo ") + cp0::system_upgrade::kPhaseUpdate + " > " + quoted_log + "; " +
                             apt + "update >> " + quoted_log},
            output, 600000, nullptr);
        if (code != 0) return {code, cp0::system_upgrade::failure(output, "apt-update")};
        code = cp0_process_commands::capture_argv_with_timeout(
            {"sh", "-c", std::string("echo ") + cp0::system_upgrade::kPhaseUpgrade + " >> " + quoted_log + "; " +
                             apt + "-y -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold "
                                   "upgrade >> " + quoted_log},
            output, 7200000, nullptr);
        if (code != 0) return {code, cp0::system_upgrade::failure(output, "apt-upgrade")};
        return {0, cp0::system_upgrade::summary(read_file(log))};
    }

    // Returns 1 if systemd automatic time sync (NTP) is enabled, 0 if disabled,
    // negative on failure to query.
    static int ntp_get()
    {
        int enabled = 0;
        return cp0_timedate_get_ntp(&enabled) == 0 ? enabled : -1;
    }
};

extern "C" void init_osinfo(void)
{
    static cp0::SignalRegistration<decltype(cp0_signal_osinfo_api)> registration;
    static cp0::SignalRegistration<decltype(cp0_signal_timedate_api)> timedate_registration;
    auto osinfo = std::make_shared<OsInfoSystem>();
    registration.replace(cp0_signal_osinfo_api, [osinfo](std::list<std::string> arg, std::function<void(int, std::string)> callback) {
        osinfo->api_call(std::move(arg), std::move(callback));
    });
    timedate_registration.replace(cp0_signal_timedate_api, [osinfo](std::list<std::string> arg, std::function<void(int, std::string)> callback) {
        osinfo->api_call(std::move(arg), std::move(callback));
    });
}

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "cp0_lvgl_app.h"
#include "hal_lvgl_bsp.h"
#include "../cp0_app_internal_utils.h"
#include "../cp0_callback_contract.hpp"
#include "../cp0_osinfo_codec.hpp"
#include "../cp0_osinfo_contract.hpp"
#include "../cp0_signal_registration.hpp"
#include "../cp0_sync_signal.hpp"
#include "../cp0_update_job.hpp"
#include "../cp0_system_upgrade.hpp"
#include "cp0_process_commands.hpp"
#include "cp0_timedate_client.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fstream>
#include <list>
#include <iterator>
#include <memory>
#include <mutex>
#include <random>
#include <pwd.h>
#include <sstream>
#include <string>
#include <utility>
#include <unistd.h>

class OsInfoSystem
{
public:
    using callback_t = std::function<void(int, std::string)>;
    using arg_t = std::list<std::string>;

    OsInfoSystem()
    {
        operations_.read_eth_info = [](bool, cp0_eth_info_t *info) {
            return network_default_info_read(info);
        };
        operations_.list_networks = [](cp0_netif_info_t *entries, int capacity, int *count) {
            return cp0_network_list(entries, capacity, count);
        };
        operations_.read_account_info = account_info_read;
        operations_.set_time = [](const std::string &timestamp) { return time_set(timestamp.c_str()); };
        operations_.read_local_time = read_local_time;
        operations_.random_u32 = random_u32;
        operations_.get_ntp = ntp_get;
        operations_.set_ntp = ntp_set;
    }

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
                    [log, running](const std::atomic<bool> &) {
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
        if (command == "UpdateJobCancel") {
            const auto id = arg.size() > 1 ? *std::next(arg.begin()) : std::string();
            cp0::callback::invoke(callback, jobs_.cancel(id) ? 0 : -1, {});
            return;
        }
        const cp0::osinfo::Result result = cp0::osinfo::dispatch(arg, operations_);
        cp0::callback::invoke(callback, result.code, result.payload);
    }

    static int api_simple(const arg_t &arg, std::string *out = nullptr)
    {
        int result = -1;
        if (out) out->clear();
        cp0::signal::invoke_noexcept([&] { cp0_signal_osinfo_api(arg, [&](int code, std::string data) {
            result = code;
            if (out)
                *out = std::move(data);
        }); });
        return result;
    }

private:
    cp0::osinfo::Operations operations_;
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


    static uint32_t random_u32() noexcept
    {
        try {
            std::random_device source;
            return static_cast<uint32_t>(source());
        } catch (...) {
            const auto seed = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            static thread_local std::mt19937 fallback(seed);
            return fallback();
        }
    }

    static int read_local_time(std::tm *value)
    {
        if (!value)
            return -1;
        const std::time_t now = std::time(nullptr);
        return localtime_r(&now, value) ? 0 : (errno ? -errno : -1);
    }

    static int network_default_info_read(cp0_eth_info_t *info)
    {
        cp0::osinfo::clear_eth_info(info);
        if (!info)
            return -1;

        char output[2048] = {};
        const char *ip_argv[] = {"ip", "-4", "addr", "show", "eth0", nullptr};
        if (cp0_process_capture_argv(ip_argv, output, sizeof(output)) == 0) {
            std::istringstream lines(output);
            std::string line;
            while (std::getline(lines, line)) {
                auto pos = line.find("inet ");
                if (pos == std::string::npos)
                    continue;
                std::istringstream iss(line.substr(pos + 5));
                std::string ip;
                iss >> ip;
                cp0_copy_string(info->ipv4, sizeof(info->ipv4), ip.empty() ? "N/A" : ip);
                break;
            }
        }

        const char *route_argv[] = {"ip", "route", nullptr};
        if (cp0_process_capture_argv(route_argv, output, sizeof(output)) == 0) {
            std::istringstream lines(output);
            std::string line;
            while (std::getline(lines, line)) {
                if (line.find("default") == std::string::npos || line.find("eth0") == std::string::npos)
                    continue;
                std::istringstream iss(line);
                std::string word;
                while (iss >> word) {
                    if (word == "via") {
                        std::string gw;
                        if (iss >> gw)
                            cp0_copy_string(info->gateway, sizeof(info->gateway), gw);
                        break;
                    }
                }
                break;
            }
        }

        cp0_file_read_first_line("/sys/class/net/eth0/address", info->mac, sizeof(info->mac));
        return 0;
    }

    static int account_info_read(cp0_account_info_t *info)
    {
        if (!info)
            return -1;
        std::memset(info, 0, sizeof(*info));
        const char *user = getlogin();
        if (!user || !user[0]) {
            struct passwd *pw = getpwuid(getuid());
            user = pw ? pw->pw_name : nullptr;
        }
        cp0_copy_cstr(info->user, sizeof(info->user), user && user[0] ? user : "N/A");

        char host[sizeof(info->hostname)] = {};
        if (gethostname(host, sizeof(host) - 1) == 0 && host[0])
            cp0_copy_cstr(info->hostname, sizeof(info->hostname), host);
        else
            cp0_copy_cstr(info->hostname, sizeof(info->hostname), "N/A");
        return 0;
    }

    static int time_set(const char *timestamp)
    {
        return cp0_timedate_set_time(timestamp);
    }

    // Returns 1 if systemd automatic time sync (NTP) is enabled, 0 if disabled,
    // negative on failure to query.
    static int ntp_get()
    {
        int enabled = 0;
        return cp0_timedate_get_ntp(&enabled) == 0 ? enabled : -1;
    }

    static int ntp_set(bool enable)
    {
        return cp0_timedate_set_ntp(enable ? 1 : 0);
    }

public:
    static int api_eth_info(const char *command, cp0_eth_info_t *info)
    {
        if (!info)
            return -1;
        std::string data;
        int ret = api_simple({command}, &data);
        if (ret == 0 && !cp0::osinfo::decode_eth_info_strict(data, info))
            ret = -1;
        return ret;
    }

    static int api_account_info(cp0_account_info_t *info)
    {
        if (!info)
            return -1;
        std::string data;
        int ret = api_simple({"AccountInfoRead"}, &data);
        if (ret == 0 && !cp0::osinfo::decode_account_info_strict(data, info))
            ret = -1;
        return ret;
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

extern "C" int cp0_network_default_info_read(cp0_eth_info_t *info)
{
    return OsInfoSystem::api_eth_info("NetworkDefaultInfoRead", info);
}

extern "C" int cp0_eth_info_read(cp0_eth_info_t *info)
{
    return OsInfoSystem::api_eth_info("EthInfoRead", info);
}

extern "C" int cp0_account_info_read(cp0_account_info_t *info)
{
    return OsInfoSystem::api_account_info(info);
}

extern "C" int cp0_time_set(const char *timestamp)
{
    return OsInfoSystem::api_simple({"TimeSet", timestamp ? timestamp : ""});
}

extern "C" int cp0_time_ntp_get(void)
{
    return OsInfoSystem::api_simple({"NtpGet"});
}

extern "C" int cp0_time_ntp_set(int enable)
{
    return OsInfoSystem::api_simple({"NtpSet", enable ? "1" : "0"});
}

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "cp0_lvgl_app.h"
#include "hal_lvgl_bsp.h"
#include "../cp0_app_internal_utils.h"
#include "cp0_process_commands.hpp"
#include "cp0_desktop_exec_policy.hpp"
#include "cp0_external_app_runner.hpp"
#include "cp0_process_lifecycle.hpp"
#include "../cp0_process_api_contract.hpp"
#include "../cp0_signal_registration.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>
#include <utility>

class ProcessSystem
{
public:
    using callback_t = std::function<void(int, std::string)>;
    using arg_t = std::list<std::string>;

    void api_call(arg_t arg, callback_t callback)
    {
        try {
        const std::string cmd = arg.empty() ? "" : arg.front();
        if (cmd == "ExecBlocking") {
            bool keep_root = false;
            const auto *exec_path = cp0_process_api_contract::argument_at(arg, 1);
            const auto *keep_root_text = cp0_process_api_contract::argument_at(arg, 2);
            if (!cp0_process_api_contract::has_exact_arguments(arg, 3) || !exec_path ||
                exec_path->empty() || !keep_root_text ||
                !cp0_process_api_contract::parse_bool(*keep_root_text, keep_root))
                return invalid(callback);
            report(callback,
                   cp0_external_app_runner::run(exec_path->c_str(), keep_root),
                   "");
        } else if (cmd == "Spawn") {
            bool keep_root = false;
            const auto *exec_path = cp0_process_api_contract::argument_at(arg, 1);
            const auto *flag = cp0_process_api_contract::argument_at(arg, 2);
            if (!cp0_process_api_contract::has_exact_arguments(arg, 3) || !exec_path ||
                exec_path->empty() || !flag ||
                !cp0_process_api_contract::parse_bool(*flag, keep_root))
                return invalid(callback);
            cp0_pid_t pid = cp0_process_lifecycle::spawn(exec_path->c_str(), keep_root);
            report(callback, pid < 0 ? -1 : 0, std::to_string(pid));
        } else if (cmd == "Stop") {
            int pid = 0;
            const auto *text = cp0_process_api_contract::argument_at(arg, 1);
            if (!cp0_process_api_contract::has_exact_arguments(arg, 2) || !text ||
                !cp0_process_api_contract::parse_pid(*text, pid))
                return invalid(callback);
            cp0_process_lifecycle::stop(static_cast<cp0_pid_t>(pid));
            report(callback, 0, "");
        } else if (cmd == "CaptureArgv") {
            if (!cp0_process_api_contract::has_at_least_arguments(arg, 2))
                return invalid(callback);
            std::string output;
            int ret = cp0_process_commands::capture_argv(
                cp0_process_api_contract::arguments_from(arg, 1), output);
            report(callback, ret, output);
        } else if (cmd == "DesktopExecIsSafe") {
            const auto *exec = cp0_process_api_contract::argument_at(arg, 1);
            if (!cp0_process_api_contract::has_exact_arguments(arg, 2) || !exec || exec->empty())
                return invalid(callback);
            char reason[128] = {};
            int safe = cp0_desktop_exec_is_safe(exec->c_str(), reason, sizeof(reason));
            report(callback, safe ? 0 : -1, reason);
        } else if (cmd == "Shutdown") {
            if (!cp0_process_api_contract::has_exact_arguments(arg, 1)) return invalid(callback);
            std::string reason;
            const int result = system_shutdown(reason);
            report(callback, result, reason);
        } else if (cmd == "Reboot") {
            if (!cp0_process_api_contract::has_exact_arguments(arg, 1)) return invalid(callback);
            std::string reason;
            const int result = system_reboot(reason);
            report(callback, result, reason);
        } else {
            report(callback, -1, "unknown process api command");
        }
        } catch (...) {
            report(callback, -1, "process api failure");
        }
    }

    // Without a TTY systemctl defaults to --check-inhibitors=auto, which skips
    // the check; ask for it so a running package install or update can veto.
    // On refusal the first line systemctl printed comes back as the reason.
    static int request_power_state(const char *verb, std::string &reason)
    {
        std::string output;
        const int result = cp0_process_commands::capture_argv(
            {"/bin/sh", "-c", std::string("exec systemctl ") + verb + " --check-inhibitors=yes 2>&1"}, output);
        reason.clear();
        if (result == 0) return 0;
        reason = output.substr(0, output.find('\n'));
        if (reason.empty()) reason = "systemctl " + std::string(verb) + " failed (" + std::to_string(result) + ")";
        return result;
    }

    int system_shutdown(std::string &reason)
    {
        std::printf("[CP0] shutdown\n");
        const int result = request_power_state("poweroff", reason);
        // Only darken the panel once logind has accepted the request; a refused
        // poweroff must leave a lit, usable screen rather than one that only
        // looks switched off.
        if (result == 0) cp0_backlight_write(0);
        return result;
    }

    int system_reboot(std::string &reason)
    {
        std::printf("[CP0] reboot\n");
        return request_power_state("reboot", reason);
    }

private:
    static void report(callback_t callback, int code, const std::string &data)
    {
        cp0_process_api_contract::invoke_callback_safely(callback, code, data);
    }

    static void invalid(const callback_t &callback)
    {
        report(callback, -1, "invalid process api arguments");
    }

public:
    static int api_simple(const arg_t &arg, std::string *data = nullptr)
    {
        int result = -1;
        cp0_signal_process_api(arg, [&](int code, std::string out) {
            result = code;
            if (data)
                *data = std::move(out);
        });
        return result;
    }
};

extern "C" void init_process(void)
{
    cp0_process_api_contract::invoke_c_api_void([] {
        static auto process = std::make_shared<ProcessSystem>();
        static cp0::SignalRegistration<decltype(cp0_signal_process_api)> registration;
        const auto active_process = process;
        registration.replace(cp0_signal_process_api, [active_process](std::list<std::string> arg, std::function<void(int, std::string)> callback) {
            active_process->api_call(std::move(arg), std::move(callback));
        });
    });
}

extern "C" int cp0_desktop_exec_is_safe(const char *exec, char *reason, int reason_size)
{
    return cp0_process_api_contract::invoke_c_api([&] {
        const cp0_desktop_exec_policy::Result result = cp0_desktop_exec_policy::evaluate(exec);
        if (!result.allowed) cp0_copy_string(reason, reason_size, result.reason);
        return result.allowed ? 1 : 0;
    });
}

extern "C" void cp0_system_shutdown(void)
{
    cp0_process_api_contract::invoke_c_api_void(
        [] { ProcessSystem::api_simple({"Shutdown"}); });
}

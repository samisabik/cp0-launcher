/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "cp0_lvgl_app.h"
#include "hal_lvgl_bsp.h"

#include "../cp0/cp0_desktop_exec_policy.hpp"
#include "../cp0/cp0_process_commands.hpp"
#include "../cp0/cp0_process_lifecycle.hpp"
#include "../cp0_app_internal_utils.h"
#include "../cp0_process_api_contract.hpp"
#include "../cp0_signal_registration.hpp"
#include "sdl_external_app_runner.hpp"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class ProcessSystem
{
public:
    using callback_t = std::function<void(int, std::string)>;
    using arg_t = std::list<std::string>;

    void api_call(arg_t args, callback_t callback)
    {
        try {
        const std::string command = args.empty() ? "" : args.front();
        if (command == "ExecBlocking") {
            bool keep_root = false;
            const auto *executable = cp0_process_api_contract::argument_at(args, 1);
            const auto *keep_root_text = cp0_process_api_contract::argument_at(args, 2);
            if (!cp0_process_api_contract::has_exact_arguments(args, 3) || !executable ||
                executable->empty() || !keep_root_text ||
                !cp0_process_api_contract::parse_bool(*keep_root_text, keep_root))
                return invalid(callback);
            (void)keep_root;
            report(callback, sdl_external_app_runner::run(executable->c_str()), "");
        } else if (command == "Spawn") {
            bool keep_root = false;
            const auto *executable = cp0_process_api_contract::argument_at(args, 1);
            const auto *flag = cp0_process_api_contract::argument_at(args, 2);
            if (!cp0_process_api_contract::has_exact_arguments(args, 3) || !executable ||
                executable->empty() || !flag ||
                !cp0_process_api_contract::parse_bool(*flag, keep_root))
                return invalid(callback);
            const cp0_pid_t pid = cp0_process_lifecycle::spawn(executable->c_str(), keep_root);
            report(callback, pid < 0 ? -1 : 0, std::to_string(pid));
        } else if (command == "Stop") {
            int pid = 0;
            const auto *text = cp0_process_api_contract::argument_at(args, 1);
            if (!cp0_process_api_contract::has_exact_arguments(args, 2) || !text ||
                !cp0_process_api_contract::parse_pid(*text, pid))
                return invalid(callback);
            cp0_process_lifecycle::stop(static_cast<cp0_pid_t>(pid));
            report(callback, 0, "");
        } else if (command == "CaptureArgv") {
            if (!cp0_process_api_contract::has_at_least_arguments(args, 2))
                return invalid(callback);
            std::string output;
            const int result = cp0_process_commands::capture_argv(
                cp0_process_api_contract::arguments_from(args, 1), output);
            report(callback, result, output);
        } else if (command == "DesktopExecIsSafe") {
            const auto *exec = cp0_process_api_contract::argument_at(args, 1);
            if (!cp0_process_api_contract::has_exact_arguments(args, 2) || !exec || exec->empty())
                return invalid(callback);
            char reason[128] = {};
            const int safe = cp0_desktop_exec_is_safe(exec->c_str(), reason, sizeof(reason));
            report(callback, safe ? 0 : -1, reason);
        } else if (command == "Shutdown") {
            if (!cp0_process_api_contract::has_exact_arguments(args, 1)) return invalid(callback);
            system_shutdown();
            report(callback, 0, "");
        } else if (command == "Reboot") {
            if (!cp0_process_api_contract::has_exact_arguments(args, 1)) return invalid(callback);
            system_reboot();
            report(callback, 0, "");
        } else {
            report(callback, -1, "unknown process api command");
        }
        } catch (...) {
            report(callback, -1, "process api failure");
        }
    }

    static int api_simple(const arg_t &args, std::string *data = nullptr)
    {
        int result = -1;
        cp0_signal_process_api(args, [&](int code, std::string output) {
            result = code;
            if (data) *data = std::move(output);
        });
        return result;
    }

private:
    static void report(const callback_t &callback, int code, const std::string &data)
    {
        cp0_process_api_contract::invoke_callback_safely(callback, code, data);
    }

    static void invalid(const callback_t &callback)
    {
        report(callback, -1, "invalid process api arguments");
    }

    static void system_shutdown()
    {
        std::printf("[SDL] shutdown requested (simulated no-op)\n");
    }

    static void system_reboot()
    {
        std::printf("[SDL] reboot requested (simulated no-op)\n");
    }
};

extern "C" void init_process(void)
{
    cp0_process_api_contract::invoke_c_api_void([] {
        static auto process = std::make_shared<ProcessSystem>();
        static cp0::SignalRegistration<decltype(cp0_signal_process_api)> registration;
        const auto active_process = process;
        registration.replace(cp0_signal_process_api,
            [active_process](std::list<std::string> args,
                      std::function<void(int, std::string)> callback) {
                active_process->api_call(std::move(args), std::move(callback));
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

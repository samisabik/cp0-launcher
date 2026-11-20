/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "launch.h"

#include <utility>

app::app(std::string name, std::string exec, bool terminal, bool sysplause,
         TerminalHelpFactory help_factory)
    : Name(std::move(name)), Exec(exec)
{
    launch = [exec = std::move(exec), terminal, sysplause, help_factory](Launch *owner) {
        if (!owner || exec.empty()) return;
        if (terminal) owner->launch_Exec_in_terminal(exec, sysplause, help_factory);
        else owner->launch_Exec(exec);
    };
}

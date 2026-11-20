/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "cp0_process_lifecycle.hpp"

#include "../cp0_external_process_group.hpp"
#include "cp0_process_commands.hpp"

#include <cstdio>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cp0_process_lifecycle {

cp0_pid_t spawn(const char *command, bool keep_root)
{
    cp0_process_group::enable_subreaper();
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setpgid(0, 0);
        if (keep_root)
            execlp("/bin/sh", "sh", "-c", command, static_cast<char *>(nullptr));
        else
            cp0_process_commands::exec_shell_as_configured_user(command);
        _exit(127);
    }
    setpgid(pid, pid);
    return static_cast<cp0_pid_t>(pid);
}

void stop(cp0_pid_t pid)
{
    if (pid <= 0) return;
    if (!cp0_process_group::terminate_and_reap(static_cast<pid_t>(pid),
                                               static_cast<pid_t>(pid))) {
        std::fprintf(stderr,
                     "[process] failed to stop and reap pgid=%d\n",
                     static_cast<int>(pid));
    }
}

} // namespace cp0_process_lifecycle

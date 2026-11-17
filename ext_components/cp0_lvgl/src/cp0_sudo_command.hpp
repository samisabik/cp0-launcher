/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>
#include <vector>

namespace cp0_sudo {

inline std::vector<std::string> invalidation_command()
{
    return {"/usr/bin/sudo", "-k"};
}

inline std::vector<std::string> validation_command()
{
    return {"/usr/bin/sudo", "-S", "-v"};
}

// `sudo -n true` succeeds when the user may run commands without a password
// (a NOPASSWD rule), in which case the password prompt is skipped entirely.
// `sudo -v` cannot be used for that check: it asks for a password whenever any
// of the user's rules needs one.
inline std::vector<std::string> passwordless_probe_command()
{
    return {"/usr/bin/sudo", "-n", "true"};
}

inline std::vector<std::string> passwordless_command(const std::vector<std::string> &arguments)
{
    std::vector<std::string> command = {"/usr/bin/sudo", "-n", "--"};
    command.insert(command.end(), arguments.begin(), arguments.end());
    return command;
}

inline std::vector<std::string> execution_command(const std::vector<std::string> &arguments,
                                                  bool use_login_shell,
                                                  const std::string &shell)
{
    std::vector<std::string> command = {"/usr/bin/sudo", "-S", "--"};
    if (use_login_shell) {
        command.push_back(shell);
        command.push_back("-c");
        command.push_back(arguments.front());
    } else {
        command.insert(command.end(), arguments.begin(), arguments.end());
    }
    return command;
}

} // namespace cp0_sudo

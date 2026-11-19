/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "cp0_admin_command.hpp"

#include <cassert>
#include <list>
#include <string>

namespace {

std::list<std::string> argv_for(std::list<std::string> args)
{
    std::list<std::string> argv;
    const bool ok = cp0::admin::command_argv(args, argv);
    return ok ? argv : std::list<std::string>{"<rejected>"};
}

bool rejected(std::list<std::string> args)
{
    return argv_for(std::move(args)) == std::list<std::string>{"<rejected>"};
}

} // namespace

int main()
{
    assert((argv_for({"SshSet", "1"}) == std::list<std::string>{"systemctl", "enable", "--now", "ssh"}));
    assert((argv_for({"SshSet", "0"}) == std::list<std::string>{"systemctl", "disable", "--now", "ssh"}));
    assert(rejected({"SshSet"}));
    assert(rejected({"SshSet", "on"}));
    assert(rejected({"SshSet", "1", "extra"}));

    const auto hostname = argv_for({"HostnameSet", "cp0-lab"});
    assert(hostname.size() == 3 && hostname.front() == "/bin/sh");
    assert(hostname.back().rfind("hostnamectl set-hostname cp0-lab && ", 0) == 0);
    assert(hostname.back().find("/etc/hosts") != std::string::npos);
    assert(rejected({"HostnameSet", ""}));
    assert(rejected({"HostnameSet", "-cp0"}));
    assert(rejected({"HostnameSet", "cp0-"}));
    assert(rejected({"HostnameSet", "cp0;reboot"}));
    assert(rejected({"HostnameSet", "cp0 lab"}));
    assert(rejected({"HostnameSet", "cp0.lan"}));
    assert(rejected({"HostnameSet", std::string(64, 'a')}));
    assert(!rejected({"HostnameSet", std::string(63, 'a')}));

    assert((argv_for({"NtpSet", "0"}) == std::list<std::string>{"timedatectl", "set-ntp", "false"}));
    assert((argv_for({"TimezoneSet", "Europe/London"}) ==
            std::list<std::string>{"timedatectl", "set-timezone", "Europe/London"}));
    assert(rejected({"TimezoneSet", "../etc/passwd"}));
    assert((argv_for({"TailscaleSet", "0"}) == std::list<std::string>{"tailscale", "down"}));
    assert(rejected({"MicGainSet", "15"}));
    assert(!rejected({"MicGainSet", "14"}));
    assert(!rejected({"TimeSet", "2026-02-28 12:30:00"}));
    assert(rejected({"TimeSet", "2026-02-30 12:30:00"}));
    assert(rejected({"TimeSet", "2026-02-28 12:30:00'; reboot; '"}));
    assert(rejected({"Unknown"}));
    assert(rejected({}));
    return 0;
}

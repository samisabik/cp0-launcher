/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "cp0_system_upgrade.hpp"

#include <cassert>
#include <string>

using namespace cp0::system_upgrade;

int main()
{
    assert(progress("") == "checking:0");
    assert(progress(std::string(kPhaseUpdate) + "\nHit:1 http://deb.debian.org\ndlstatus:1:42.5:Retrieving\n") ==
           "checking:42");

    const std::string upgrading = std::string(kPhaseUpdate) + "\ndlstatus:1:100:done\n" + kPhaseUpgrade +
                                  "\nReading package lists...\n";
    assert(progress(upgrading) == "checking:100");
    assert(progress(upgrading + "dlstatus:3:61.2:Retrieving file 3 of 5\n") == "downloading:61");
    assert(progress(upgrading + "dlstatus:5:100:done\npmstatus:libc6:12.5:Preparing libc6\n") == "installing:12");
    // A pmstatus line before the upgrade phase is not an install.
    assert(progress(std::string(kPhaseUpdate) + "\npmstatus:x:50:y\n") == "checking:0");
    assert(status_percent("pmstatus:pkg:250:x") == 100);
    assert(status_percent("garbage") == 0);

    assert(summary("Calculating upgrade...\n0 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n") ==
           "0 upgraded");
    assert(summary("12 upgraded, 1 newly installed, 0 to remove and 2 not upgraded.\n") == "12 upgraded");
    assert(summary("nothing here\n") == "done");

    assert(failure("W: warning\nE: Could not get lock /var/lib/dpkg/lock|x\n", "apt-upgrade") ==
           "Could not get lock /var/lib/dpkg/lock x");
    assert(failure("sudo: a password is required\n", "apt-update") == "sudo: a password is required");
    assert(failure("", "apt-update") == "apt-update");
    return 0;
}

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "../main/ui/settings/settings_system_model.hpp"

#include <cassert>

int main()
{
    using namespace settings_system;

    const UpdateStatusInfo running = parse_update_status("running");
    assert(!running.terminal);
    assert(running.stage == "running");
    assert(running.progress == -1);

    const UpdateStatusInfo checking = parse_update_status("checking:0");
    assert(!checking.terminal);
    assert(checking.stage == "checking");
    assert(checking.progress == 0);

    const UpdateStatusInfo installing = parse_update_status("installing:100");
    assert(!installing.terminal);
    assert(installing.stage == "installing");
    assert(installing.progress == 100);

    assert(parse_update_status("downloading:-15").progress == 0);
    assert(parse_update_status("downloading:140").progress == 100);
    assert(parse_update_status("downloading:not-a-number").progress == -1);
    assert(parse_update_status("downloading:12%").progress == -1);
    assert(parse_update_status("downloading:999999999999999999999999").progress == -1);
    assert(parse_update_status("downloading:").progress == -1);

    const UpdateStatusInfo succeeded = parse_update_status("succeeded:installing:100");
    assert(succeeded.terminal);
    assert(succeeded.stage == "succeeded");
    assert(succeeded.progress == -1);

    const UpdateStatusInfo failed = parse_update_status("failed:apt-upgrade:100");
    assert(failed.terminal);
    assert(failed.stage == "failed");

    assert(parse_update_status("cancelled").terminal);
    assert(!parse_update_status("").terminal);
}

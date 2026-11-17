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

    const UpdateStatusInfo current = parse_update_status(
        "succeeded:up-to-date|version=1.4.0|commit=abc1234");
    assert(current.terminal);
    assert(current.availability_known);
    assert(!current.available);
    assert(current.version == "1.4.0");
    assert(current.commit == "abc1234");
    assert(current.progress == static_cast<int>(UpdateStatusInfo::Progress::Unknown));
    assert(current.stage == "succeeded");

    const UpdateStatusInfo available = parse_update_status(
        "succeeded:update-available", "version=1.5.0|commit=Add launcher update dialog");
    assert(available.terminal);
    assert(available.availability_known);
    assert(available.available);
    assert(available.version == "1.5.0");
    assert(available.commit == "Add launcher update dialog");

    const UpdateStatusInfo zero_progress = parse_update_status("checking:0");
    assert(!zero_progress.terminal);
    assert(zero_progress.progress == 0);
    assert(zero_progress.stage == "checking");

    const UpdateStatusInfo full_progress = parse_update_status("installing:100");
    assert(!full_progress.terminal);
    assert(full_progress.progress == 100);
    assert(full_progress.stage == "installing");

    assert(parse_update_status("downloading:-15").progress == 0);
    assert(parse_update_status("verifying:140").progress == 100);
    assert(parse_update_status("repairing:not-a-number").progress == -1);
    assert(parse_update_status("restarting:999999999999999999999999").progress == -1);

    const UpdateStatusInfo legacy_success = parse_update_status("succeeded:1.3.0");
    assert(legacy_success.terminal);
    assert(legacy_success.availability_known);
    assert(!legacy_success.available);
    assert(legacy_success.version == "1.3.0");

    const UpdateStatusInfo empty_success = parse_update_status("succeeded");
    assert(empty_success.terminal);
    assert(empty_success.version.empty());

    const UpdateStatusInfo legacy_stage = parse_update_status("recovering:install");
    assert(!legacy_stage.terminal);
    assert(legacy_stage.stage == "recovering");
    assert(legacy_stage.progress == static_cast<int>(UpdateStatusInfo::Progress::Unknown));
}

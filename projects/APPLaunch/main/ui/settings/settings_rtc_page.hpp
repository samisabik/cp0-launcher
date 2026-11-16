/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "lvgl_components.hpp"
#include "settings_tree_types.hpp"

void settings_rtc_ntp_api(int command, void *data) noexcept;

// Why "set manually" must refuse activation right now, or nullptr when it may
// open.  The editor re-checks the same state before it writes.
const ActivationBlock *settings_rtc_manual_edit_block() noexcept;

// Re-read Network Time instead of reusing the per-process cache, so the status
// icon and the gate agree with what the editor will see.  This is a local
// D-Bus property fetch (~35ms on hardware), not a blocking wait.
void settings_rtc_refresh_ntp() noexcept;

// One page that edits yyyy-mm-dd hh:mm and writes the system clock and RTC.
std::unique_ptr<DComponens::LvglComponensBase> settings_rtc_editor_page_factory(
    lv_obj_t *parent,
    const NodeIter &parent_node,
    std::function<void()> back_callback);

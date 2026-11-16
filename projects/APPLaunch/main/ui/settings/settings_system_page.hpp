/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "settings_tree_types.hpp"

#include <functional>
#include <memory>

#define LVGL_COMPONENTS_ROLLER1_ONLY
#include "lvgl_components.hpp"
#undef LVGL_COMPONENTS_ROLLER1_ONLY

// System -> updates: check for a launcher update, install it, or refresh the
// system package lists, using the osinfo update jobs.
std::unique_ptr<DComponens::LvglComponensBase> settings_updates_page_factory(
    lv_obj_t *parent,
    const NodeIter &page_node,
    std::function<void()> back_callback);

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

// System -> hardware: read-only list of what is attached, refreshed on ENT.
std::unique_ptr<DComponens::LvglComponensBase> settings_hardware_page_factory(
    lv_obj_t *parent,
    const NodeIter &page_node,
    std::function<void()> back_callback);

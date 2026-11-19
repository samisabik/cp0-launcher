/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "settings_tree_types.hpp"
#include "lvgl_components.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Tailscale on/off toggle (SettingEntry api) and its status page.
void tailscale_enabled_api(int cmd, void *data);
std::unique_ptr<DComponens::LvglComponensBase> settings_tailscale_page_factory(lv_obj_t *parent,
                                                                               const NodeIter &page_node,
                                                                               std::function<void()> on_back);

// Kernel, uptime, host, user and network addresses for the About page.
std::vector<std::string> settings_system_lines();

// IPv4 address of an interface, or an empty string when it has none.
std::string settings_interface_ipv4(const char *name);
std::string settings_hostname();

// SSH server on/off toggle: `systemctl is-active ssh`, switched through the admin helper.
void ssh_server_api(int cmd, void *data);

// 24h / 12h clock picker, stored under settings_prefs::kClock12h.
void append_clock_format_options(Tree &tree, const NodeIter &parent);
std::unique_ptr<DComponens::LvglComponensBase> settings_clock_format_page_factory(lv_obj_t *parent,
                                                                                  const NodeIter &page_node,
                                                                                  std::function<void()> on_back);

// Microphone PGA gain, 0..42 dB in 3 dB steps; children are the option labels.
void append_mic_gain_options(Tree &tree, const NodeIter &parent);
// screen > accent: the options are cp0::theme::kAccentPresets, in order.
void append_accent_options(Tree &tree, const NodeIter &parent);
std::unique_ptr<DComponens::LvglComponensBase> settings_accent_page_factory(lv_obj_t *parent,
                                                                            const NodeIter &page_node,
                                                                            std::function<void()> on_back);

std::unique_ptr<DComponens::LvglComponensBase> settings_mic_gain_page_factory(lv_obj_t *parent,
                                                                              const NodeIter &page_node,
                                                                              std::function<void()> on_back);

// Time zone picker; children are zone names.
void append_time_zone_options(Tree &tree, const NodeIter &parent);
std::unique_ptr<DComponens::LvglComponensBase> settings_time_zone_page_factory(lv_obj_t *parent,
                                                                               const NodeIter &page_node,
                                                                               std::function<void()> on_back);

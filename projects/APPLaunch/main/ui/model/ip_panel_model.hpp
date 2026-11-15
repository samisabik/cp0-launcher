/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct IpInterfaceInfo {
    std::string name;
    std::string address;
    std::string netmask;
    bool up = false;

    bool operator==(const IpInterfaceInfo &other) const
    {
        return name == other.name && address == other.address && netmask == other.netmask && up == other.up;
    }
};

constexpr bool ip_panel_refresh_callback_allowed(bool timer_is_current,
                                                 bool refresh_enabled) noexcept
{
    return timer_is_current && refresh_enabled;
}

template <typename CurrentTarget, typename TrackedRoot>
constexpr bool ip_panel_root_callback_allowed(
    CurrentTarget current_target, TrackedRoot tracked_root) noexcept
{
    return current_target != nullptr && current_target == tracked_root;
}

template <typename Background, typename List>
constexpr bool ip_panel_view_ready(Background background, List list) noexcept
{
    return background && list;
}

template <typename EventTarget, typename CurrentTarget>
constexpr bool ip_panel_owned_delete_callback_allowed(EventTarget event_target,
                                                       CurrentTarget current_target) noexcept
{
    return event_target != nullptr && event_target == current_target;
}

class IpPanelModel
{
public:
    void replace(std::vector<IpInterfaceInfo> interfaces);
    // Returns true when the list changed, so the view only rebuilds then.
    bool apply_refresh(bool loaded, std::vector<IpInterfaceInfo> interfaces);
    bool select_previous();
    bool select_next();

    const std::vector<IpInterfaceInfo> &interfaces() const { return interfaces_; }
    size_t selected_index() const { return selected_index_; }
    size_t visible_offset(size_t visible_count) const;

private:
    std::vector<IpInterfaceInfo> interfaces_;
    size_t selected_index_ = 0;
};

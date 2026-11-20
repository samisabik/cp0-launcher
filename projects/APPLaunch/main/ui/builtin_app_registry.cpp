/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "builtin_app_registry.hpp"

#include "app_display_order.hpp"
#include "app_registry.h"
#include "cli_terminal_help.hpp"
#include "launch.h"
#include "ui.h"
#include "generated/page_app.h"
#include "launcher_platform.hpp"
#include "python_terminal_help.hpp"
#include "settings/settings_page.hpp"
#include <limits>
#include <vector>

namespace {

using BuiltinAppAppender = void (*)(std::list<app> &apps, const AppDescriptor &desc);

struct BuiltinAppRegistration {
    AppDescriptor desc;
    const char *exec;
    bool terminal;
    bool sysplause;
    BuiltinAppAppender append;
    TerminalHelpFactory help_factory = nullptr;
};

template <class PageT>
void append_page_app(std::list<app> &apps, const AppDescriptor &desc)
{
    apps.emplace_back(desc.label, page_v<PageT>);
}

void append_cli_app(std::list<app> &apps, const AppDescriptor &desc)
{
    apps.emplace_back(desc.label, page_v<UISTPage>, create_cli_help);
}

std::string resolved_exec(const BuiltinAppRegistration &registration)
{
    std::string exec = registration.exec ? registration.exec : "";
    if (!exec.empty() && exec.front() == '@') return launcher_platform::path(exec.substr(1));
    return exec;
}

void append_builtin_app(std::list<app> &apps, const BuiltinAppRegistration &registration)
{
    if (registration.append) {
        registration.append(apps, registration.desc);
        return;
    }
    apps.emplace_back(registration.desc.label,
                      resolved_exec(registration),
                      registration.terminal,
                      registration.sysplause,
                      registration.help_factory);
}

constexpr BuiltinAppRegistration BUILTIN_APPS[] = {
    {{"Settings", "app_Setting", false, true},
     nullptr, false, true, append_page_app<UISettingTreePage>},
    {{"CLI", "app_CLI", false, true},
     nullptr, false, true, append_cli_app},
    {{"Python", "app_Python", true, false},
     "python3", true, false, nullptr, create_python_help},
    {{"Files", "app_Files", true, false},
     nullptr, false, true, append_page_app<UIFilesPage>},
#if defined(__linux__) && !defined(HAL_PLATFORM_SDL)
    {{"SSH", "app_SSH", true, false},
     nullptr, false, true, append_page_app<UISSHPage>},
    {{"IP Panel", "app_IP_Panel", true, false},
     nullptr, false, true, append_page_app<UIIpPanelPage>},
    {{"Power", "app_Power", true, false},
     nullptr, false, true, append_page_app<UIPowerPage>},
    {{"Spectrum", "app_Spectrum", true, false},
     nullptr, false, true, append_page_app<UISpectrumPage>},
    {{"Rec", "app_Rec", true, false},
     nullptr, false, true, append_page_app<UIRecPage>},
    {{"SDR", "app_SDR", true, false},
     nullptr, false, true, append_page_app<UISdrPage>},
    {{"Audio", "app_Audio", true, false},
     nullptr, false, true, append_page_app<UIAudioPage>},
    {{"ADSB", "app_ADSB", true, false},
     nullptr, false, true, append_page_app<UIAdsbPage>},
    {{"Sat", "app_Sat", true, false},
     nullptr, false, true, append_page_app<UISatPage>},
    {{"Sensors", "app_Sensors", true, false},
     nullptr, false, true, append_page_app<UISensorsPage>},
    {{"Keys", "app_Keys", true, false},
     nullptr, false, true, append_page_app<UIKeysPage>},
    {{"Fox", "app_Fox", true, false},
     nullptr, false, true, append_page_app<UIFoxPage>},
    {{"IR", "app_IR", true, false},
     nullptr, false, true, append_page_app<UIIrPage>},
    {{"Pins", "app_Pins", true, false},
     nullptr, false, true, append_page_app<UIPinsPage>},
    {{"Tailnet", "app_Tailnet", true, false},
     nullptr, false, true, append_page_app<UITailnetPage>},
    {{"Air", "app_Air", true, false},
     nullptr, false, true, append_page_app<UIAirPage>},
    {{"Portal", "app_Portal", true, false},
     nullptr, false, true, append_page_app<UIPortalPage>},
    {{"Slime", "app_Slime", true, false},
     nullptr, false, true, append_page_app<UISlimePage>},
    {{"Tracker", "app_Tracker", true, false},
     nullptr, false, true, append_page_app<UITrackerPage>},
#endif
};

} // namespace

const AppDescriptor *launcher_app_registry_entries(std::size_t *count)
{
    static const std::vector<AppDescriptor> descriptors = [] {
        std::vector<AppDescriptor> result;
        result.reserve(sizeof(BUILTIN_APPS) / sizeof(BUILTIN_APPS[0]));
        for (const auto &registration : BUILTIN_APPS) result.push_back(registration.desc);
        return result;
    }();
    if (count) *count = descriptors.size();
    return descriptors.data();
}

void launcher_append_enabled_builtin_apps(std::list<app> &apps)
{
    for (const auto &registration : BUILTIN_APPS)
        if (launcher_app_registry_is_enabled(registration.desc)) append_builtin_app(apps, registration);
}

void launcher_sort_app_display_order(std::list<app> &apps)
{
    constexpr int installable_app_order = std::numeric_limits<int>::max();
    const auto rank = [](const std::string &name) {
        const int pinned = launcher_home_first_page_order(name);
        return pinned >= 0 ? pinned : installable_app_order;
    };
    apps.sort([&rank](const app &left, const app &right) { return rank(left.Name) < rank(right.Name); });
}

bool launcher_builtin_app_owns_exec(const std::string &exec)
{
    for (const auto &registration : BUILTIN_APPS) {
        const std::string builtin_exec = resolved_exec(registration);
        if (!builtin_exec.empty() && exec == builtin_exec) return true;
    }
    return false;
}

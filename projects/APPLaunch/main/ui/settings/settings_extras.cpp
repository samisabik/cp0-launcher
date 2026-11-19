/*
 * SPDX-License-Identifier: MIT
 */

#include "settings_extras.hpp"

#include "cp0_theme.hpp"
#include "hal_lvgl_bsp.h"
#include "settings_choice_page.hpp"
#include "settings_prefs.hpp"
#include "settings_static_info_page.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cstdlib>
#include <list>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <thread>
#include <tuple>
#include <unistd.h>

namespace {

constexpr const char *kTailscale = "/usr/bin/tailscale";
constexpr int kMicGainSteps = 15;

// Common zones, west to east. timedatectl validates the name again.
constexpr const char *kTimeZones[] = {
    "America/Los_Angeles", "America/Denver",  "America/Chicago", "America/New_York", "America/Sao_Paulo",
    "UTC",                 "Europe/London",   "Europe/Lisbon",   "Europe/Paris",     "Europe/Berlin",
    "Europe/Athens",       "Europe/Istanbul", "Africa/Cairo",    "Asia/Dubai",       "Asia/Kolkata",
    "Asia/Bangkok",        "Asia/Singapore",  "Asia/Shanghai",   "Asia/Tokyo",       "Australia/Sydney",
    "Pacific/Auckland",
};

bool capture(std::list<std::string> argv, std::string &output)
{
    int result = -1;
    argv.push_front("CaptureArgv");
    try {
        cp0_signal_process_api(std::move(argv), [&](int code, std::string data) {
            result = code;
            output = std::move(data);
        });
    } catch (...) {
        return false;
    }
    return result == 0;
}

// Value of a string field in flat JSON output; enough for tailscale status.
std::string json_string(const std::string &json, const std::string &key)
{
    const std::string needle = "\"" + key + "\"";
    std::size_t at = json.find(needle);
    if (at == std::string::npos) return {};
    at = json.find(':', at + needle.size());
    if (at == std::string::npos) return {};
    at = json.find('"', at);
    if (at == std::string::npos) return {};
    const std::size_t end = json.find('"', at + 1);
    return end == std::string::npos ? std::string() : json.substr(at + 1, end - at - 1);
}

std::string tailscale_status_json()
{
    std::string json;
    capture({kTailscale, "status", "--json", "--peers=false"}, json);
    return json;
}

std::atomic<bool> g_tailscale_pending{false};

std::string interface_ipv4(const char *name)
{
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) return {};
    std::string address;
    for (ifaddrs *entry = list; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET || std::string(entry->ifa_name) != name)
            continue;
        char text[INET_ADDRSTRLEN] = {};
        const auto *in = reinterpret_cast<const sockaddr_in *>(entry->ifa_addr);
        if (inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text))) address = text;
        break;
    }
    freeifaddrs(list);
    return address;
}

std::string uptime_text()
{
    struct sysinfo info{};
    if (sysinfo(&info) != 0) return "unknown";
    const long minutes = info.uptime / 60;
    char text[32];
    if (minutes >= 1440)
        std::snprintf(text, sizeof(text), "%ldd %ldh", minutes / 1440, (minutes / 60) % 24);
    else
        std::snprintf(text, sizeof(text), "%ldh %02ldm", minutes / 60, minutes % 60);
    return text;
}

std::string read_first_line(const char *path)
{
    std::FILE *file = std::fopen(path, "r");
    if (!file) return {};
    char line[256] = {};
    const bool ok = std::fgets(line, sizeof(line), file) != nullptr;
    std::fclose(file);
    std::string text = ok ? line : "";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

int current_mic_gain_index()
{
    std::string output;
    if (!capture({"/usr/bin/amixer", "-c0", "sget", "ADCL PGA"}, output)) return -1;
    // "  Mono: 12 [86%] [36.00dB]"
    const std::size_t at = output.find("Mono:");
    if (at == std::string::npos) return -1;
    const int value = std::atoi(output.c_str() + at + 5);
    return value >= 0 && value < kMicGainSteps ? value : -1;
}

int current_time_zone_index()
{
    std::string zone;
    if (!capture({"/usr/bin/timedatectl", "show", "-p", "Timezone", "--value"}, zone)) return -1;
    while (!zone.empty() && (zone.back() == '\n' || zone.back() == ' ')) zone.pop_back();
    for (std::size_t i = 0; i < sizeof(kTimeZones) / sizeof(kTimeZones[0]); ++i)
        if (zone == kTimeZones[i]) return static_cast<int>(i);
    return -1;
}

} // namespace

void tailscale_enabled_api(int cmd, void *data)
{
    if (cmd == SettingApiReadFlag || cmd == SettingApiReadFlagTimeStart) {
        const bool running = json_string(tailscale_status_json(), "BackendState") == "Running";
        if (cmd == SettingApiReadFlag && data) *static_cast<bool *>(data) = running;
        if (cmd == SettingApiReadFlagTimeStart && data) {
            auto *result = static_cast<SettingApiReadFlagTimeStartData *>(data);
            std::get<0>(*result) = running;
            if (std::get<1>(*result)) std::get<1>(*result)->store(g_tailscale_pending.load());
        }
        return;
    }
    if (cmd != SettingApiActivate || g_tailscale_pending.exchange(true)) return;
    // The CLI takes a few hundred ms and tailscale up several seconds; keep
    // both off the UI thread.
    std::thread([] {
        const bool running = json_string(tailscale_status_json(), "BackendState") == "Running";
        settings_run_admin({"TailscaleSet", running ? "0" : "1"});
        g_tailscale_pending.store(false);
    }).detach();
}

namespace {

std::vector<std::string> tailscale_lines()
{
    const std::string json = tailscale_status_json();
    std::string state = json_string(json, "BackendState");
    for (char &c : state) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::size_t self_at = json.find("\"Self\"");
    const std::string self_name =
        self_at == std::string::npos ? std::string() : json_string(json.substr(self_at), "HostName");
    const std::string suffix = json_string(json, "MagicDNSSuffix");
    const std::string ip = interface_ipv4("tailscale0");
    return {
        "state      " + (state.empty() ? std::string("not installed") : state),
        "address    " + (ip.empty() ? std::string("-") : ip),
        "name       " + (self_name.empty() ? std::string("-") : self_name),
        "tailnet    " + (suffix.empty() ? std::string("-") : suffix),
    };
}

struct TailscaleLines {
    std::mutex mutex;
    std::vector<std::string> lines;
    bool fresh = false;
};

} // namespace

std::unique_ptr<DComponens::LvglComponensBase> settings_tailscale_page_factory(lv_obj_t *parent,
                                                                               const NodeIter &page_node,
                                                                               std::function<void()> on_back)
{
    // Open with placeholders and fill in from a worker: the tailscale CLI is
    // too slow to run on the UI thread.
    settings_t12b::about_help::Content content{
        "tailscale", {"state      reading", "address    -", "name       -", "tailnet    -"}};
    auto shared = std::make_shared<TailscaleLines>();
    std::thread([shared] {
        std::vector<std::string> lines = tailscale_lines();
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->lines = std::move(lines);
        shared->fresh = true;
    }).detach();
    auto page = std::make_unique<LvSettingStaticInfoPage3>(parent, page_node, std::move(on_back), std::move(content));
    page->set_lines_provider([shared](std::vector<std::string> &lines) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (!shared->fresh) return false;
        shared->fresh = false;
        lines = shared->lines;
        return true;
    });
    return page;
}

std::string settings_interface_ipv4(const char *name)
{
    return name ? interface_ipv4(name) : std::string();
}

std::string settings_hostname()
{
    char host[65] = {};
    if (gethostname(host, sizeof(host) - 1) != 0) return {};
    return host;
}

namespace {

std::atomic<bool> g_ssh_pending{false};
std::atomic<bool> g_ssh_active{false};

bool ssh_is_active()
{
    std::string output;
    std::list<std::string> argv{"CaptureArgv", "/usr/bin/systemctl", "is-active", "ssh"};
    try {
        cp0_signal_process_api(std::move(argv), [&](int, std::string data) { output = std::move(data); });
    } catch (...) {
        return false;
    }
    while (!output.empty() && (output.back() == '\n' || output.back() == ' ')) output.pop_back();
    return output == "active";
}

} // namespace

void ssh_server_api(int cmd, void *data)
{
    if (cmd == SettingApiReadFlag || cmd == SettingApiReadFlagTimeStart) {
        const bool pending = g_ssh_pending.load();
        if (!pending) g_ssh_active.store(ssh_is_active());
        if (cmd == SettingApiReadFlag && data) *static_cast<bool *>(data) = g_ssh_active.load();
        if (cmd == SettingApiReadFlagTimeStart && data) {
            auto *result = static_cast<SettingApiReadFlagTimeStartData *>(data);
            std::get<0>(*result) = g_ssh_active.load();
            if (std::get<1>(*result)) std::get<1>(*result)->store(pending);
        }
        return;
    }
    if (cmd != SettingApiActivate || g_ssh_pending.exchange(true)) return;
    const bool next = !g_ssh_active.load();
    g_ssh_active.store(next);
    // enable --now waits for the unit to start; keep it off the UI thread.
    std::thread([next] {
        if (!settings_run_admin({"SshSet", next ? "1" : "0"}).empty()) g_ssh_active.store(!next);
        g_ssh_pending.store(false);
    }).detach();
}

void append_clock_format_options(Tree &tree, const NodeIter &parent)
{
    tree.append_child(parent, SettingEntry{"24h"});
    tree.append_child(parent, SettingEntry{"12h"});
}

std::unique_ptr<DComponens::LvglComponensBase> settings_clock_format_page_factory(lv_obj_t *parent,
                                                                                  const NodeIter &page_node,
                                                                                  std::function<void()> on_back)
{
    return std::make_unique<LvSettingChoicePage3>(
        parent, page_node, std::move(on_back), [] { return settings_prefs::clock_12h() ? 1 : 0; },
        [](int index) {
            if (index != 0 && index != 1) return std::string("invalid format");
            return settings_prefs::set_int(settings_prefs::kClock12h, index) ? std::string()
                                                                               : std::string("save failed");
        });
}

std::vector<std::string> settings_system_lines()
{
    std::vector<std::string> lines;
    utsname uts{};
    if (uname(&uts) == 0) lines.push_back(std::string("kernel     ") + uts.release);
    lines.push_back("uptime     " + uptime_text());
    char host[64] = {};
    if (gethostname(host, sizeof(host) - 1) == 0) lines.push_back(std::string("host       ") + host);
    if (const passwd *user = getpwuid(getuid())) lines.push_back(std::string("user       ") + user->pw_name);
    const std::string model = read_first_line("/proc/device-tree/model");
    if (!model.empty()) lines.push_back("board      " + model);
    for (const char *name : {"wlan0", "wlan1", "eth0", "tailscale0"}) {
        const std::string ip = interface_ipv4(name);
        if (ip.empty()) continue;
        std::string label = name;
        label.resize(11, ' ');
        lines.push_back(label + ip);
    }
    return lines;
}

void append_mic_gain_options(Tree &tree, const NodeIter &parent)
{
    for (int step = 0; step < kMicGainSteps; ++step)
        tree.append_child(parent, SettingEntry{"+" + std::to_string(step * 3) + " dB"});
}

std::unique_ptr<DComponens::LvglComponensBase> settings_mic_gain_page_factory(lv_obj_t *parent,
                                                                              const NodeIter &page_node,
                                                                              std::function<void()> on_back)
{
    return std::make_unique<LvSettingChoicePage3>(
        parent, page_node, std::move(on_back), [] { return current_mic_gain_index(); },
        [](int index) { return settings_run_admin({"MicGainSet", std::to_string(index)}); });
}

void append_accent_options(Tree &tree, const NodeIter &parent)
{
    for (const cp0::theme::AccentPreset &preset : cp0::theme::kAccentPresets)
        tree.append_child(parent, SettingEntry{preset.name});
}

std::unique_ptr<DComponens::LvglComponensBase> settings_accent_page_factory(lv_obj_t *parent,
                                                                            const NodeIter &page_node,
                                                                            std::function<void()> on_back)
{
    return std::make_unique<LvSettingChoicePage3>(
        parent, page_node, std::move(on_back),
        [] { return cp0::theme::accent_index(settings_prefs::get_int(cp0::theme::kAccentConfigKey, 0)); },
        [](int index) {
            if (index != cp0::theme::accent_index(index)) return std::string("unknown colour");
            return settings_prefs::set_int(cp0::theme::kAccentConfigKey, index) ? std::string()
                                                                                : std::string("save failed");
        },
        [](int index) {
            cp0::ui::apply_accent(cp0::theme::kAccentPresets[static_cast<std::size_t>(cp0::theme::accent_index(index))].color);
        });
}

void append_time_zone_options(Tree &tree, const NodeIter &parent)
{
    for (const char *zone : kTimeZones) tree.append_child(parent, SettingEntry{zone});
}

std::unique_ptr<DComponens::LvglComponensBase> settings_time_zone_page_factory(lv_obj_t *parent,
                                                                               const NodeIter &page_node,
                                                                               std::function<void()> on_back)
{
    return std::make_unique<LvSettingChoicePage3>(
        parent, page_node, std::move(on_back), [] { return current_time_zone_index(); },
        [](int index) {
            constexpr int count = static_cast<int>(sizeof(kTimeZones) / sizeof(kTimeZones[0]));
            if (index < 0 || index >= count) return std::string("invalid zone");
            return settings_run_admin({"TimezoneSet", kTimeZones[index]});
        });
}

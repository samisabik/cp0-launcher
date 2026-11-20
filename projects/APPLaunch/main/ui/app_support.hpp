/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "cp0_lvgl_app.h"
#include "lvgl.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <dirent.h>
#include <string>
#include <unistd.h>
#include <vector>

// Small helpers shared by the built-in app pages: child processes through the
// platform process API, the directories pages keep their files in, and pixel
// conversion for RGB565 canvases.
namespace app_support {

inline std::string runtime_dir()
{
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    return runtime && runtime[0] ? runtime : "/tmp";
}

inline std::string home_dir()
{
    const char *home = std::getenv("HOME");
    return home && home[0] ? home : "/tmp";
}

// Runs a shell command in the background. Returns its pid, or -1.
inline int spawn(const std::string &command)
{
    int pid = -1;
    cp0_signal_process_api({"Spawn", command, "0"}, [&](int code, std::string data) {
        if (code == 0) pid = std::atoi(data.c_str());
    });
    return pid;
}

// Stops a process started with spawn() and clears the pid.
inline void stop(int &pid)
{
    if (pid <= 0) return;
    const int target = pid;
    pid = -1;
    cp0_signal_process_api({"Stop", std::to_string(target)}, [](int, std::string) {});
}

// True when an executable with this name is in /usr/bin, /usr/local/bin,
// /usr/sbin or /bin.
inline bool tool_present(const std::string &name)
{
    for (const char *dir : {"/usr/bin/", "/usr/local/bin/", "/usr/sbin/", "/bin/"})
        if (access((std::string(dir) + name).c_str(), X_OK) == 0) return true;
    return false;
}

// Names of the Wi-Fi adapters the kernel knows, sorted: "wlan0" is the
// built-in radio the system connects with, later ones are USB adapters.
inline std::vector<std::string> wifi_interfaces()
{
    std::vector<std::string> names;
    DIR *dir = opendir("/sys/class/net");
    if (!dir) return names;
    while (const dirent *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name[0] == '.') continue;
        if (access(("/sys/class/net/" + name + "/phy80211").c_str(), F_OK) == 0) names.push_back(name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}

// Whole file as a string; empty when it cannot be read.
// Asks the device's helper to load (or unload) the USB Wi-Fi driver, which
// stays unloaded while unused so the USB bus can sleep. The adapter shows up
// in wifi_interfaces() a few seconds after `true`. Does nothing on a device
// without the helper, where the driver is always loaded.
inline void usb_wifi(bool on)
{
    static const char *const kHelper = "/usr/local/sbin/cp0-usb-wifi";
    if (access(kHelper, X_OK) != 0) return;
    spawn(std::string("sudo -n ") + kHelper + (on ? " on" : " off") + " >/dev/null 2>&1");
}

inline std::string read_file(const std::string &path)
{
    std::string out;
    std::FILE *file = std::fopen(path.c_str(), "r");
    if (!file) return out;
    char buffer[1024];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) out.append(buffer, n);
    std::fclose(file);
    return out;
}

inline uint16_t rgb565(uint32_t rgb)
{
    return lv_color_to_u16(lv_color_hex(rgb));
}

} // namespace app_support

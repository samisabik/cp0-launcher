/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "cp0_lvgl_app.h"
#include "lvgl.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

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

// Whole file as a string; empty when it cannot be read.
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

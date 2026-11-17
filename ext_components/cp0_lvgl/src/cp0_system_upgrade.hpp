/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdlib>
#include <sstream>
#include <string>

// The system upgrade job runs `apt-get update` then `apt-get upgrade` with
// APT::Status-Fd=1, appending both to one log. kPhaseUpdate/kPhaseUpgrade lines
// mark where each step starts. These helpers turn that log into the update job
// status records the settings page already understands.
namespace cp0::system_upgrade {

constexpr const char *kPhaseUpdate = "cp0-phase:update";
constexpr const char *kPhaseUpgrade = "cp0-phase:upgrade";

inline int status_percent(const std::string &line)
{
    // dlstatus:<n>:<percent>:<text> / pmstatus:<package>:<percent>:<text>
    const std::size_t first = line.find(':');
    if (first == std::string::npos) return 0;
    const std::size_t second = line.find(':', first + 1);
    if (second == std::string::npos) return 0;
    const int percent = std::atoi(line.c_str() + second + 1);
    return percent < 0 ? 0 : percent > 100 ? 100 : percent;
}

// "checking:<n>" while package lists refresh, then "downloading:<n>" and
// "installing:<n>" from the upgrade's own status lines.
inline std::string progress(const std::string &log)
{
    std::istringstream lines(log);
    std::string line;
    bool upgrade = false;
    std::string stage = "checking";
    int percent = 0;
    while (std::getline(lines, line)) {
        if (line == kPhaseUpdate) {
            upgrade = false;
            stage = "checking";
            percent = 0;
        } else if (line == kPhaseUpgrade) {
            upgrade = true;
            stage = "checking";
            percent = 100;
        } else if (line.rfind("dlstatus:", 0) == 0) {
            stage = upgrade ? "downloading" : "checking";
            percent = status_percent(line);
        } else if (upgrade && line.rfind("pmstatus:", 0) == 0) {
            stage = "installing";
            percent = status_percent(line);
        }
    }
    return stage + ":" + std::to_string(percent);
}

// apt's "N upgraded, M newly installed, ..." line reduced to "N upgraded".
inline std::string summary(const std::string &log)
{
    std::istringstream lines(log);
    std::string line;
    std::string result = "done";
    while (std::getline(lines, line)) {
        const std::size_t at = line.find(" upgraded, ");
        if (at != std::string::npos && at > 0) result = line.substr(0, at) + " upgraded";
    }
    return result;
}

// The last apt error ("E: ...") from the command output, else its last
// non-empty line, cleaned so it fits a job status record.
inline std::string failure(const std::string &output, const std::string &fallback)
{
    std::istringstream lines(output);
    std::string line;
    std::string error;
    std::string last;
    while (std::getline(lines, line)) {
        if (line.rfind("E: ", 0) == 0) error = line.substr(3);
        if (!line.empty()) last = line;
    }
    std::string result = error.empty() ? last : error;
    if (result.empty()) return fallback;
    for (char &character : result)
        if (character == '|' || character == '\r') character = ' ';
    if (result.size() > 96) result.resize(96);
    return result;
}

} // namespace cp0::system_upgrade

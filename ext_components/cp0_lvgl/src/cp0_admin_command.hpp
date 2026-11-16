/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <iterator>
#include <list>
#include <string>

namespace cp0::admin {

inline bool is_hostname(const std::string &value)
{
    if (value.empty() || value.size() > 63 || value.front() == '-' || value.back() == '-') return false;
    return std::all_of(value.begin(), value.end(),
                       [](unsigned char c) { return std::isalnum(c) || c == '-'; });
}

inline bool is_timestamp(const std::string &value)
{
    const bool valid_shape = value.size() == 19 && value[4] == '-' && value[7] == '-' && value[10] == ' ' &&
                             value[13] == ':' && value[16] == ':';
    bool digits_valid = valid_shape;
    for (size_t i = 0; digits_valid && i < value.size(); ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) continue;
        digits_valid = std::isdigit(static_cast<unsigned char>(value[i])) != 0;
    }
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    const bool parsed = digits_valid && std::sscanf(value.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour,
                                                    &minute, &second) == 6;
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    static constexpr int days_per_month[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int max_day = month == 2 ? days_per_month[month] + (leap ? 1 : 0)
                                   : (month >= 1 && month <= 12 ? days_per_month[month] : 0);
    return parsed && year >= 1970 && month >= 1 && month <= 12 && day >= 1 && day <= max_day && hour >= 0 &&
           hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 59;
}

// Maps an allow-listed admin request to the argv that runs through sudo.
// Returns false for an unknown command or an invalid argument.
inline bool command_argv(const std::list<std::string> &args,
                         const std::function<std::string(const char *)> &file_path,
                         std::list<std::string> &argv)
{
    argv.clear();
    if (args.empty()) return false;
    const std::string command = args.front();
    const std::string value = args.size() > 1 ? *std::next(args.begin()) : std::string();
    const auto helper = [&file_path](const char *name) { return file_path ? file_path(name) : std::string(name); };
    const bool binary = value == "0" || value == "1";

    if (command == "AdbSet") {
        if (args.size() != 2 || !binary) return false;
        argv = {helper("adb_helper"), "set", value};
    } else if (command == "AdbAuthorize") {
        if (value.size() < 680 || value.size() > 2048 || value.find('\n') != std::string::npos ||
            value.find('\r') != std::string::npos)
            return false;
        argv = {helper("adb_helper"), "authorize", value};
    } else if (command == "AdbRevoke") {
        const bool valid = value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
                               return std::isdigit(c) || (c >= 'a' && c <= 'f');
                           });
        if (!valid) return false;
        argv = {helper("adb_helper"), "revoke", value};
    } else if (command == "AdbClearAuthorizations") {
        argv = {helper("adb_helper"), "clear-authorizations"};
    } else if (command == "NtpSet") {
        argv = {"timedatectl", "set-ntp", value == "1" ? "true" : "false"};
    } else if (command == "TimezoneSet") {
        // Zone names are paths under /usr/share/zoneinfo such as Europe/London.
        const bool valid = !value.empty() && value.size() <= 64 && value.front() != '/' &&
                           value.find("..") == std::string::npos &&
                           std::all_of(value.begin(), value.end(), [](unsigned char c) {
                               return std::isalnum(c) || c == '/' || c == '_' || c == '-' || c == '+';
                           });
        if (args.size() != 2 || !valid) return false;
        argv = {"timedatectl", "set-timezone", value};
    } else if (command == "TailscaleSet") {
        if (args.size() != 2 || !binary) return false;
        argv = {"tailscale", value == "1" ? "up" : "down"};
    } else if (command == "SshSet") {
        if (args.size() != 2 || !binary) return false;
        argv = {"systemctl", value == "1" ? "enable" : "disable", "--now", "ssh"};
    } else if (command == "HostnameSet") {
        if (args.size() != 2 || !is_hostname(value)) return false;
        // is_hostname() limits the value to [A-Za-z0-9-], so it is safe inside
        // the script.  The 127.0.1.1 line keeps sudo able to resolve the new name.
        argv = {"/bin/sh", "-c",
                "hostnamectl set-hostname " + value +
                    " && if grep -q '^127\\.0\\.1\\.1' /etc/hosts; then sed -i 's/^127\\.0\\.1\\.1.*/127.0.1.1\\t" +
                    value + "/' /etc/hosts; else printf '127.0.1.1\\t%s\\n' " + value + " >> /etc/hosts; fi"};
    } else if (command == "MicGainSet") {
        // ES8389 ADC PGA steps of 3 dB, 0..14 (0 to +42 dB); stored so it survives a reboot.
        const bool valid = !value.empty() && value.size() <= 2 &&
                           std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); }) &&
                           std::stoi(value) <= 14;
        if (args.size() != 2 || !valid) return false;
        argv = {"/bin/sh", "-c",
                "amixer -c0 -q sset 'ADCL PGA' " + value + " && amixer -c0 -q sset 'ADCR PGA' " + value +
                    " && alsactl store 0"};
    } else if (command == "TimeSet") {
        if (!is_timestamp(value)) return false;
        argv = {"/bin/sh", "-c", "date -s '" + value + "'; hwclock -w"};
    } else {
        return false;
    }
    return true;
}

} // namespace cp0::admin

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>

// Logic for the Portal page: the config files, scripts and served page that
// stand up an open test access point on wlan1 (hostapd + dnsmasq + a static
// captive page), plus the parsing the page uses to read back its state. Free
// of LVGL and process access so it can be unit-tested.
//
// The served page is a plain "hello" with no form and no logging: it exists
// to exercise client DHCP and OS captive-portal detection, not to collect
// anything a client types.
namespace portal {

// The adapter the AP runs on. wlan0 stays the system Wi-Fi; wlan1 is the USB
// adapter used for diagnostics.
inline constexpr const char *kInterface = "wlan1";
inline constexpr const char *kGatewayIp = "10.0.0.1";
inline constexpr int kPortalPort = 8080;
inline constexpr const char *kDefaultSsid = "cp0 portal";
inline constexpr int kDefaultChannel = 6;

// hostapd.conf for an open 2.4 GHz AP. The SSID is cut to 32 bytes and any
// control character is replaced with a space, so it cannot break the file.
std::string hostapd_conf(const std::string &ssid, int channel);

// dnsmasq.conf: DHCP on kInterface and a wildcard A record to the gateway, so
// captive-portal probes resolve to the portal. bind-interfaces keeps dnsmasq
// off wlan0, so the system resolver is untouched. Lease and pid files live in
// `dir`.
std::string dnsmasq_conf(const std::string &dir);

// The static page every path returns.
std::string index_html();
// A stdlib HTTP server (no Flask) that serves index.html for every request on
// 0.0.0.0:kPortalPort, and appends one tab-separated line per request to
// <dir>/visits.log: UTC time, client IP, path, User-Agent. Connection
// metadata only; nothing the client submits is read or stored.
std::string serve_py(const std::string &dir);

// Brings kInterface up as the AP: addr, DNAT of 80/443 to the portal, then
// dnsmasq, the portal server and hostapd. Run as root (the page prefixes
// `sudo -n`). Files are read from `dir`.
std::string start_script(const std::string &dir);
// Reverses start_script and returns kInterface to NetworkManager.
std::string stop_script(const std::string &dir);

// Return code from a run log that ends with a "#rc N" marker, or -1 when the
// marker is absent (the script is still running).
int parse_rc(const std::string &log);
// Active DHCP leases in a dnsmasq lease file, i.e. connected clients.
int count_leases(const std::string &leasefile);
// Trimmed last non-empty line of the log, for an error message.
std::string last_line(const std::string &log);

} // namespace portal

/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

// Pure helpers for the Air page: parsing Wi-Fi scan results from nmcli and
// Bluetooth device lists from the platform API, channel maths, the congestion
// score and the curve drawn on the channel map. Free of LVGL and process
// access so they can be unit-tested.
namespace air {

enum class Band { Unknown, G24, G5 };
enum class Sort { Signal, Channel, Name };

struct Network {
    std::string ssid;     // empty for a hidden network
    int channel = 0;
    int signal = 0;       // 0..100 as NetworkManager reports it
    std::string security; // "wpa3", "wpa2", "wpa", "wep", "open"
};

// Fields of one `nmcli -t` line. Fields are separated by ':'; a ':' or '\'
// inside a value is escaped with a backslash.
std::vector<std::string> split_nmcli_line(const std::string &line);

// Output of `nmcli -t -f SSID,CHAN,SIGNAL,SECURITY device wifi list`.
// Lines without four fields or without a usable channel are skipped.
std::vector<Network> parse_nmcli(const std::string &output);
// With an interface name the scan uses that adapter only; without one nmcli
// lists what every Wi-Fi adapter sees, so a network shows once per adapter.
std::string scan_cmd(const std::string &ifname = {});
// The adapter after `current` in `interfaces`, wrapping; the first one when
// `current` is not listed, and empty when there are none.
std::string next_interface(const std::vector<std::string> &interfaces, const std::string &current);

// "WPA2 WPA3" -> "wpa3", "WPA1 WPA2" -> "wpa2", "" or "--" -> "open".
std::string security_label(const std::string &nmcli_security);

// Channels 1..14 are 2.4 GHz and 32..177 are 5 GHz. A channel number alone
// cannot tell 6 GHz apart, so 6 GHz networks are filed under the same numbers.
Band band_of(int channel);
const char *band_label(Band band);

// Approximate conversion between a 0..100 signal and dBm: dBm = signal/2 - 100,
// the inverse of the mapping the title-row Wi-Fi bars use.
int percent_to_dbm(int percent);

std::vector<Network> in_band(const std::vector<Network> &networks, Band band);
void sort_networks(std::vector<Network> &networks, Sort sort);
Sort next_sort(Sort sort);
const char *sort_label(Sort sort);

// Share of a 22 MHz wide channel that another one `channels` away overlaps
// (channels are 5 MHz apart): 1 on the same channel, 0 from 5 channels away.
float overlap(float channels);

// Congestion per 2.4 GHz channel: every network adds signal/100 times its
// overlap with that channel. Index 1..13 is used; index 0 stays 0.
constexpr int kChannels24 = 13;
std::array<float, kChannels24 + 1> congestion(const std::vector<Network> &networks);
// The least congested of the non-overlapping channels 1, 6 and 11.
int quietest_channel(const std::array<float, kChannels24 + 1> &scores);

// Channel map curve: a parabola `peak` high at the network's channel that
// reaches zero two channels to either side (about 22 MHz wide in total).
constexpr float kHalfWidthChannels = 2.0f;
float curve_height(int network_channel, float at_channel, float peak);
// Peak height in pixels for a 0..100 signal on a map max_height pixels tall.
float peak_height(int signal, float max_height);

// One bar of the 5 GHz chart: a channel in use, how many networks are on it
// and the strongest signal there. Sorted by channel; when more than max_bars
// channels are in use the weakest are dropped.
struct ChannelBar {
    int channel = 0;
    int count = 0;
    int signal = 0;
};
std::vector<ChannelBar> channel_bars(const std::vector<Network> &networks, std::size_t max_bars);

// Clips to `columns` characters without cutting a UTF-8 sequence, and pads
// with spaces to exactly that many columns when pad is set.
std::string fit(const std::string &text, std::size_t columns, bool pad = true);
std::string display_name(const Network &network);
// "cp0-lab      ch 6   wpa3   -42" in fixed columns of 13, 7, 7 and 4.
std::string row_text(const Network &network);

struct BleDevice {
    std::string address;
    std::string name;
    int rssi = 0; // dBm; 0 when the adapter has no reading
    bool connected = false;
    bool paired = false;
};

// Device lines of the platform Bluetooth API:
// address<TAB>rssi<TAB>connected<TAB>paired<TAB>trusted<TAB>name
std::vector<BleDevice> parse_bt_devices(const std::string &payload);
// Status line "powered<TAB>address<TAB>discoverable<TAB>alias". Returns false
// when the payload is not such a line.
bool parse_bt_status(const std::string &payload, bool &powered);
// Strongest first; devices without a reading last, then by name.
void sort_ble(std::vector<BleDevice> &devices);
// The name, or the address when the name is empty or only repeats the address.
std::string ble_label(const BleDevice &device);
// 0..1 for the bar: -100 dBm and below is empty, -40 dBm and above is full.
float rssi_fraction(int rssi);
// "kitchen speaker        paired   -58" in fixed columns of 22, 7 and 4.
std::string ble_row_text(const BleDevice &device);

} // namespace air

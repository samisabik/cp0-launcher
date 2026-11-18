/*
 * SPDX-License-Identifier: MIT
 */

#include "air_model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace air {

namespace {

std::string lower(std::string text)
{
    for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string upper(std::string text)
{
    for (char &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

// Whole decimal number with an optional sign; false for anything else.
bool parse_int(const std::string &text, int &out)
{
    if (text.empty() || text.size() > 9) return false;
    std::size_t pos = text[0] == '-' ? 1 : 0;
    if (pos == text.size()) return false;
    for (std::size_t i = pos; i < text.size(); ++i)
        if (text[i] < '0' || text[i] > '9') return false;
    out = std::atoi(text.c_str());
    return true;
}

std::vector<std::string> lines_of(const std::string &text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
        start = end + 1;
    }
    return lines;
}

std::vector<std::string> split(const std::string &line, char separator)
{
    std::vector<std::string> fields(1);
    for (const char c : line) {
        if (c == separator) fields.emplace_back();
        else fields.back() += c;
    }
    return fields;
}

std::string right(const std::string &text, std::size_t columns)
{
    return text.size() >= columns ? text : std::string(columns - text.size(), ' ') + text;
}

} // namespace

std::vector<std::string> split_nmcli_line(const std::string &line)
{
    std::vector<std::string> fields(1);
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\\' && i + 1 < line.size()) {
            fields.back() += line[++i];
        } else if (c == ':') {
            fields.emplace_back();
        } else {
            fields.back() += c;
        }
    }
    return fields;
}

std::string security_label(const std::string &nmcli_security)
{
    const std::string text = upper(nmcli_security);
    if (text.find("WPA3") != std::string::npos || text.find("SAE") != std::string::npos) return "wpa3";
    if (text.find("WPA2") != std::string::npos || text.find("RSN") != std::string::npos) return "wpa2";
    if (text.find("WPA") != std::string::npos) return "wpa";
    if (text.find("WEP") != std::string::npos) return "wep";
    if (text.find("802.1X") != std::string::npos) return "802.1x";
    if (text.find("OWE") != std::string::npos) return "owe";
    return "open";
}

std::vector<Network> parse_nmcli(const std::string &output)
{
    std::vector<Network> networks;
    for (const std::string &line : lines_of(output)) {
        const std::vector<std::string> fields = split_nmcli_line(line);
        if (fields.size() != 4) continue;
        Network network;
        int signal = 0;
        if (!parse_int(fields[1], network.channel) || !parse_int(fields[2], signal)) continue;
        if (band_of(network.channel) == Band::Unknown) continue;
        network.ssid = fields[0];
        network.signal = std::clamp(signal, 0, 100);
        network.security = security_label(fields[3]);
        networks.push_back(std::move(network));
    }
    return networks;
}

std::string scan_cmd()
{
    return "nmcli -t -f SSID,CHAN,SIGNAL,SECURITY device wifi list --rescan auto";
}

Band band_of(int channel)
{
    if (channel >= 1 && channel <= 14) return Band::G24;
    if (channel >= 32 && channel <= 177) return Band::G5;
    return Band::Unknown;
}

int centre_mhz(int channel)
{
    switch (band_of(channel)) {
    case Band::G24: return channel == 14 ? 2484 : 2407 + 5 * channel;
    case Band::G5: return 5000 + 5 * channel;
    case Band::Unknown: break;
    }
    return 0;
}

const char *band_label(Band band)
{
    switch (band) {
    case Band::G24: return "2.4 ghz";
    case Band::G5: return "5 ghz";
    case Band::Unknown: break;
    }
    return "";
}

int percent_to_dbm(int percent)
{
    return std::clamp(percent, 0, 100) / 2 - 100;
}

int dbm_to_percent(int dbm)
{
    return std::clamp(2 * (dbm + 100), 0, 100);
}

std::vector<Network> in_band(const std::vector<Network> &networks, Band band)
{
    std::vector<Network> out;
    for (const Network &network : networks)
        if (band_of(network.channel) == band) out.push_back(network);
    return out;
}

void sort_networks(std::vector<Network> &networks, Sort sort)
{
    std::stable_sort(networks.begin(), networks.end(), [sort](const Network &a, const Network &b) {
        if (sort == Sort::Channel && a.channel != b.channel) return a.channel < b.channel;
        if (sort == Sort::Name) {
            // Hidden networks have no name and go last.
            if (a.ssid.empty() != b.ssid.empty()) return b.ssid.empty();
            const std::string left = lower(a.ssid);
            const std::string right_name = lower(b.ssid);
            if (left != right_name) return left < right_name;
        }
        if (a.signal != b.signal) return a.signal > b.signal;
        if (a.ssid != b.ssid) return a.ssid < b.ssid;
        return a.channel < b.channel;
    });
}

Sort next_sort(Sort sort)
{
    return sort == Sort::Signal ? Sort::Channel : sort == Sort::Channel ? Sort::Name : Sort::Signal;
}

const char *sort_label(Sort sort)
{
    switch (sort) {
    case Sort::Signal: return "signal";
    case Sort::Channel: return "channel";
    case Sort::Name: return "name";
    }
    return "";
}

float overlap(float channels)
{
    const float share = 1.0f - 5.0f * std::fabs(channels) / 22.0f;
    return share > 0.0f ? share : 0.0f;
}

std::array<float, kChannels24 + 1> congestion(const std::vector<Network> &networks)
{
    std::array<float, kChannels24 + 1> scores{};
    for (const Network &network : networks) {
        if (band_of(network.channel) != Band::G24) continue;
        const float weight = static_cast<float>(std::clamp(network.signal, 0, 100)) / 100.0f;
        for (int channel = 1; channel <= kChannels24; ++channel)
            scores[static_cast<std::size_t>(channel)] +=
                weight * overlap(static_cast<float>(channel - network.channel));
    }
    return scores;
}

int quietest_channel(const std::array<float, kChannels24 + 1> &scores)
{
    int best = 1;
    for (const int channel : {6, 11})
        if (scores[static_cast<std::size_t>(channel)] < scores[static_cast<std::size_t>(best)]) best = channel;
    return best;
}

float curve_height(int network_channel, float at_channel, float peak)
{
    const float d = (at_channel - static_cast<float>(network_channel)) / kHalfWidthChannels;
    if (d <= -1.0f || d >= 1.0f || peak <= 0.0f) return 0.0f;
    return peak * (1.0f - d * d);
}

float peak_height(int signal, float max_height)
{
    if (max_height <= 0.0f) return 0.0f;
    // A floor of 2 px keeps the weakest networks visible on the map.
    return std::max(2.0f, static_cast<float>(std::clamp(signal, 0, 100)) / 100.0f * max_height);
}

std::vector<ChannelBar> channel_bars(const std::vector<Network> &networks, std::size_t max_bars)
{
    std::vector<ChannelBar> bars;
    for (const Network &network : networks) {
        auto found = std::find_if(bars.begin(), bars.end(),
                                  [&](const ChannelBar &bar) { return bar.channel == network.channel; });
        if (found == bars.end()) {
            bars.push_back({network.channel, 1, network.signal});
        } else {
            ++found->count;
            found->signal = std::max(found->signal, network.signal);
        }
    }
    if (bars.size() > max_bars) {
        std::stable_sort(bars.begin(), bars.end(),
                         [](const ChannelBar &a, const ChannelBar &b) { return a.signal > b.signal; });
        bars.resize(max_bars);
    }
    std::sort(bars.begin(), bars.end(), [](const ChannelBar &a, const ChannelBar &b) { return a.channel < b.channel; });
    return bars;
}

std::string fit(const std::string &text, std::size_t columns, bool pad)
{
    std::string out;
    std::size_t used = 0;
    std::size_t i = 0;
    while (i < text.size() && used < columns) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        const std::size_t length =
            lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
        bool valid = length > 0 && i + length <= text.size();
        for (std::size_t k = 1; valid && k < length; ++k)
            valid = (static_cast<unsigned char>(text[i + k]) & 0xC0) == 0x80;
        if (!valid || lead < 0x20 || lead == 0x7F) {
            // Control characters and broken UTF-8 show as '?'.
            out += '?';
            ++i;
        } else {
            out.append(text, i, length);
            i += length;
        }
        ++used;
    }
    if (pad && used < columns) out.append(columns - used, ' ');
    return out;
}

std::string display_name(const Network &network)
{
    return network.ssid.empty() ? "(hidden)" : network.ssid;
}

std::string row_text(const Network &network)
{
    return fit(display_name(network), 12) + " " + fit("ch " + std::to_string(network.channel), 7) +
           fit(network.security, 7) + right(std::to_string(percent_to_dbm(network.signal)), 4);
}

std::vector<BleDevice> parse_bt_devices(const std::string &payload)
{
    std::vector<BleDevice> devices;
    for (const std::string &line : lines_of(payload)) {
        const std::vector<std::string> fields = split(line, '\t');
        if (fields.size() < 5 || fields[0].empty()) continue;
        BleDevice device;
        device.address = fields[0];
        int value = 0;
        if (parse_int(fields[1], value)) device.rssi = value;
        device.connected = fields[2] == "1";
        device.paired = fields[3] == "1";
        if (fields.size() > 5) device.name = fields[5];
        devices.push_back(std::move(device));
    }
    return devices;
}

bool parse_bt_status(const std::string &payload, bool &powered)
{
    const std::vector<std::string> fields = split(payload.substr(0, payload.find('\n')), '\t');
    int value = 0;
    if (fields.size() < 2 || !parse_int(fields[0], value)) return false;
    powered = value != 0;
    return true;
}

std::string ble_label(const BleDevice &device)
{
    // An unnamed device may carry its address with dashes as the name.
    std::string dashed = device.address;
    std::replace(dashed.begin(), dashed.end(), ':', '-');
    if (device.name.empty() || upper(device.name) == upper(dashed) || upper(device.name) == upper(device.address))
        return lower(device.address);
    return device.name;
}

void sort_ble(std::vector<BleDevice> &devices)
{
    std::stable_sort(devices.begin(), devices.end(), [](const BleDevice &a, const BleDevice &b) {
        const bool a_known = a.rssi != 0;
        const bool b_known = b.rssi != 0;
        if (a_known != b_known) return a_known;
        if (a.rssi != b.rssi) return a.rssi > b.rssi;
        const std::string left = lower(ble_label(a));
        const std::string right_name = lower(ble_label(b));
        if (left != right_name) return left < right_name;
        return a.address < b.address;
    });
}

float rssi_fraction(int rssi)
{
    if (rssi == 0) return 0.0f;
    return std::clamp((static_cast<float>(rssi) + 100.0f) / 60.0f, 0.0f, 1.0f);
}

std::string ble_row_text(const BleDevice &device)
{
    const char *state = device.connected ? "conn" : device.paired ? "paired" : "";
    return fit(ble_label(device), 22) + " " + fit(state, 7) +
           right(device.rssi == 0 ? "--" : std::to_string(device.rssi), 4);
}

} // namespace air

/*
 * SPDX-License-Identifier: MIT
 */

#include "spectrum_model.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace spectrum {

uint32_t freq_word(uint32_t hz)
{
    const uint64_t scaled = (static_cast<uint64_t>(hz) << 16) + kCrystalHz / 2;
    return static_cast<uint32_t>(scaled / kCrystalHz);
}

uint32_t bin_frequency(const Band &band, int bin)
{
    bin = std::clamp(bin, 0, kBins - 1);
    const uint64_t span = band.stop_hz - band.start_hz;
    return band.start_hz + static_cast<uint32_t>(span * static_cast<uint64_t>(bin) / (kBins - 1));
}

int rssi_dbm(uint8_t raw)
{
    const int value = raw >= 128 ? static_cast<int>(raw) - 256 : static_cast<int>(raw);
    return value / 2 - 74;
}

int level_index(int dbm)
{
    const int clamped = std::clamp(dbm, kMinDbm, kMaxDbm);
    return (clamped - kMinDbm) * (kLevels - 1) / (kMaxDbm - kMinDbm);
}

uint32_t level_color(int index)
{
    struct Stop { int at; int r, g, b; };
    static constexpr Stop kStops[] = {
        // Black up to the typical noise floor so only signals show colour.
        {0, 0x00, 0x00, 0x00},
        {12, 0x00, 0x00, 0x00},
        {28, 0x13, 0x26, 0x1B},
        {46, 0x3D, 0xDC, 0x84},
        {63, 0xFF, 0xFF, 0xFF},
    };
    index = std::clamp(index, 0, kLevels - 1);
    for (std::size_t i = 1; i < sizeof(kStops) / sizeof(kStops[0]); ++i) {
        const Stop &a = kStops[i - 1];
        const Stop &b = kStops[i];
        if (index <= b.at) {
            const int span = b.at - a.at;
            const int t = index - a.at;
            const int r = a.r + (b.r - a.r) * t / span;
            const int g = a.g + (b.g - a.g) * t / span;
            const int bl = a.b + (b.b - a.b) * t / span;
            return static_cast<uint32_t>((r << 16) | (g << 8) | bl);
        }
    }
    return 0xFFFFFF;
}

std::string format_mhz(uint32_t hz)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%03u", hz / 1000000, (hz / 1000) % 1000);
    return text;
}

std::string csv_header(const std::string &started_iso)
{
    std::string text = "# cc1101 spectrum session, started " + started_iso + "\n";
    text += "# rssi in dBm (approximate), one row per sweep\n";
    for (const Band &band : kBands) {
        char line[96];
        std::snprintf(line, sizeof(line), "# band %s start_hz=%u stop_hz=%u bins=%d\n",
                      band.label, band.start_hz, band.stop_hz, kBins);
        text += line;
    }
    text += "timestamp_ms,band";
    for (int bin = 0; bin < kBins; ++bin) text += ",b" + std::to_string(bin);
    text += "\n";
    return text;
}

std::string csv_row(int64_t timestamp_ms, const Band &band, const int8_t *dbm, std::size_t count)
{
    std::string text = std::to_string(timestamp_ms) + "," + band.label;
    for (std::size_t i = 0; i < count; ++i) text += "," + std::to_string(static_cast<int>(dbm[i]));
    text += "\n";
    return text;
}

std::vector<int32_t> normalize_durations(const std::vector<int32_t> &durations)
{
    std::vector<int32_t> out;
    out.reserve(durations.size());
    for (int32_t value : durations) {
        if (value == 0) continue;
        if (out.empty() && value < 0) continue;
        if (!out.empty() && (out.back() > 0) == (value > 0))
            out.back() += value;
        else
            out.push_back(value);
    }
    return out;
}

std::string ook_file_header(const std::string &created)
{
    return ";pulse data\n;version 1\n;timescale 1us\n;created " + created + "\n";
}

std::string ook_packet(const std::vector<int32_t> &durations, uint32_t freq_hz,
                       const std::string &received, int rssi_dbm)
{
    // rtl_433 skips marks or spaces above 100 ms.
    constexpr int32_t kMaxUs = 100000;
    const std::vector<int32_t> values = normalize_durations(durations);
    const std::size_t pulses = (values.size() + 1) / 2;
    std::string text = ";received " + received + "\n";
    text += ";ook " + std::to_string(pulses) + " pulses\n";
    text += ";freq1 " + std::to_string(freq_hz) + "\n";
    text += ";rssi " + std::to_string(rssi_dbm) + ".0 dB\n";
    for (std::size_t i = 0; i < values.size(); i += 2) {
        const int32_t mark = std::min(values[i], kMaxUs);
        const int32_t space = i + 1 < values.size() ? std::min(-values[i + 1], kMaxUs) : 0;
        text += std::to_string(mark) + " " + std::to_string(space) + "\n";
    }
    text += ";end\n";
    return text;
}

std::string sub_file_header(uint32_t freq_hz)
{
    return "Filetype: Flipper SubGhz RAW File\nVersion: 1\nFrequency: " + std::to_string(freq_hz) +
           "\nPreset: FuriHalSubGhzPresetOok650Async\nProtocol: RAW\n";
}

std::string sub_raw_lines(const std::vector<int32_t> &durations)
{
    constexpr std::size_t kPerLine = 512;
    const std::vector<int32_t> values = normalize_durations(durations);
    std::string text;
    for (std::size_t i = 0; i < values.size(); i += kPerLine) {
        text += "RAW_Data:";
        for (std::size_t j = i; j < std::min(values.size(), i + kPerLine); ++j)
            text += " " + std::to_string(values[j]);
        text += "\n";
    }
    return text;
}

bool tx_allowed(uint32_t hz)
{
    // Gated on the CC1101 synthesiser bands only (datasheet: 300-348, 387-464,
    // 779-928 MHz). Within those the chip can key a carrier; staying legal on a
    // given frequency is the operator's call.
    return (hz >= 300000000 && hz <= 348000000) || (hz >= 387000000 && hz <= 464000000) ||
           (hz >= 779000000 && hz <= 928000000);
}

Capture parse_sub(const std::string &contents)
{
    Capture capture;
    std::istringstream stream(contents);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = line.substr(0, colon);
        const std::string rest = line.substr(colon + 1);
        if (key == "Frequency") {
            capture.freq_hz = static_cast<uint32_t>(std::strtoul(rest.c_str(), nullptr, 10));
        } else if (key == "RAW_Data") {
            std::istringstream numbers(rest);
            long value = 0;
            while (numbers >> value) capture.durations.push_back(static_cast<int32_t>(value));
        }
    }
    return capture;
}

void FinderDetector::reset()
{
    for (int &h : hot_) h = 0;
    seeded_ = false;
    armed_ = true;
    quiet_ = true;
}

void FinderDetector::configure(int count, int threshold_dbm)
{
    count_ = count < 0 ? 0 : (count > kMaxChannels ? kMaxChannels : count);
    threshold_ = threshold_dbm < kMinThresholdDbm ? kMinThresholdDbm
                 : threshold_dbm > kMaxThresholdDbm ? kMaxThresholdDbm
                                                    : threshold_dbm;
    reset();
}

int FinderDetector::smoothed_dbm(int slot) const
{
    if (!seeded_ || slot < 0 || slot >= count_) return kMinDbm;
    return static_cast<int>(smooth_[slot] < 0 ? smooth_[slot] - 0.5f : smooth_[slot] + 0.5f);
}

FinderHit FinderDetector::update(const int8_t *rssi, uint32_t now_ms)
{
    FinderHit result;
    if (count_ <= 0) return result;
    if (!seeded_) {
        for (int i = 0; i < count_; ++i) {
            smooth_[i] = rssi[i];
            hot_[i] = 0;
        }
        seeded_ = true;
    }

    bool quiet = true;
    int best = -1;
    int best_dbm = kMinDbm - 1;
    for (int i = 0; i < count_; ++i) {
        // Two-tap smoothing, as the reference analyser uses, to reject a single
        // stray sample without lagging a real press.
        smooth_[i] = (smooth_[i] + rssi[i]) * 0.5f;
        if (smooth_[i] >= threshold_) {
            quiet = false;
            ++hot_[i];
        } else {
            hot_[i] = 0;
        }
        const int value = smoothed_dbm(i);
        if (hot_[i] >= kHitPasses && value > best_dbm) {
            best = i;
            best_dbm = value;
        }
    }

    if (quiet) {
        if (!quiet_) quiet_since_ms_ = now_ms;
        quiet_ = true;
    } else {
        quiet_ = false;
    }
    if (!armed_ &&
        ((quiet_ && now_ms - quiet_since_ms_ >= kQuietRearmMs) || now_ms - hit_ms_ >= kMaxHoldMs))
        armed_ = true;

    if (armed_ && best >= 0) {
        armed_ = false;
        hit_ms_ = now_ms;
        result.hit = true;
        result.slot = best;
        result.rssi_dbm = rssi[best];
    }
    return result;
}

std::string capture_name(const std::string &typed)
{
    std::string out;
    for (unsigned char c : typed) {
        if (out.size() >= kCaptureNameMax) break;
        if (c == ' ') c = '-';
        else if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') out.push_back(static_cast<char>(c));
    }
    while (!out.empty() && out.front() == '-') out.erase(out.begin());
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

} // namespace spectrum

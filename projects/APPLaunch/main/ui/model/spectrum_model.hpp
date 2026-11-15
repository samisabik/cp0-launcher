/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Pure data and arithmetic for the CC1101 spectrum page, kept free of LVGL and
// hardware access so it can be unit-tested on the host.
namespace spectrum {

constexpr uint32_t kCrystalHz = 26000000;
constexpr int kBins = 160;
constexpr int kMinDbm = -110;
constexpr int kMaxDbm = -40;
constexpr int kLevels = 64;

struct Band {
    const char *label;
    uint32_t start_hz;
    uint32_t stop_hz;
    // MDMCFG4 high nibble sets the receive filter width; the low nibble is the
    // data-rate exponent, which does not affect RSSI readings.
    uint8_t mdmcfg4;
};

// 58 kHz filter for the narrow 315/433 spans, 102 kHz for the wider 868 span
// so neighbouring bins overlap instead of leaving gaps.
inline constexpr std::array<Band, 3> kBands = {{
    {"315", 314000000, 316000000, 0xF7},
    {"433", 433050000, 434790000, 0xF7},
    {"868", 863000000, 870000000, 0xC7},
}};

// Value for the FREQ2..FREQ0 registers: f_carrier = f_xosc * FREQ / 2^16.
uint32_t freq_word(uint32_t hz);

uint32_t bin_frequency(const Band &band, int bin);

// RSSI register to dBm, using the datasheet's 74 dB offset for this data rate.
int rssi_dbm(uint8_t raw);

// 0 .. kLevels-1 position of a level on the display scale.
int level_index(int dbm);

// RGB888 colour for a level index: black through the noise floor, then dark
// green, launcher green and white.
uint32_t level_color(int index);

std::string format_mhz(uint32_t hz);

std::string csv_header(const std::string &started_iso);
std::string csv_row(int64_t timestamp_ms, const Band &band, const int8_t *dbm, std::size_t count);

// Capture formats. A capture is a list of signed durations in microseconds:
// positive while the demodulated signal is high (carrier on), negative while low.

// Merges same-sign neighbours, drops zeros and leading lows so the list starts
// high and alternates, as both file formats require.
std::vector<int32_t> normalize_durations(const std::vector<int32_t> &durations);

// rtl_433 "OOK pulse data" text, readable with `rtl_433 -r file.ook`.
std::string ook_file_header(const std::string &created);
std::string ook_packet(const std::vector<int32_t> &durations, uint32_t freq_hz,
                       const std::string &received, int rssi_dbm);

// Flipper Zero Sub-GHz RAW file.
std::string sub_file_header(uint32_t freq_hz);
std::string sub_raw_lines(const std::vector<int32_t> &durations);

// A saved capture ready for playback: a carrier frequency and the signed
// microsecond durations that drive the OOK transmitter.
struct Capture {
    uint32_t freq_hz = 0;
    std::vector<int32_t> durations;
};

// True for the licence-exempt short-range bands that permit transmission:
// 433.05-434.79 MHz and 863-870 MHz. 315 MHz is receive-only here.
bool tx_allowed(uint32_t hz);

// Parse a Flipper Sub-GHz RAW file (the format sub_file_header/sub_raw_lines
// write) back into a frequency and signed durations.
Capture parse_sub(const std::string &contents);

} // namespace spectrum

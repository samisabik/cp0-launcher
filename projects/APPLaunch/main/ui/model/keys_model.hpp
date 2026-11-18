/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Pure analysis of a sub-GHz OOK capture for the Keys page, free of LVGL and
// hardware access so it can be unit-tested on the host. A capture is the list
// of signed microsecond durations spectrum::parse_sub returns: positive while
// the carrier is on, negative while it is off.
namespace keys {

enum class Encoding { Unknown, Pwm };

// A burst needs at least this many pulses to count as a packet.
constexpr int kMinBits = 8;
// A low this many base elements long separates two packets.
constexpr int kGapTe = 5;

struct Analysis {
    Encoding encoding = Encoding::Unknown;
    // True when at least one packet decoded with every symbol inside the PWM
    // timing limits. False means bits and hex are a best effort.
    bool confident = false;
    int te_us = 0;
    int bit_count = 0;
    int packets = 0; // bursts of at least kMinBits pulses
    int repeats = 0; // packets that carry the decoded code
    // True when the pulse before the gap was taken as a sync pulse.
    bool sync = false;
    std::string bits; // '0' / '1', first received bit first
    std::string hex;  // bits left-padded to whole nibbles
    uint32_t total_us = 0;
    // The first packet that carries the decoded code, gap excluded.
    uint32_t packet_start_us = 0;
    uint32_t packet_us = 0;
    // bit_count + 1 offsets from the capture start: where each bit begins and
    // where the last one ends.
    std::vector<uint32_t> bit_edges_us;
};

// Base time element: the median of the shortest cluster of durations that
// holds a real share of the capture, so a few stray glitches do not win.
int estimate_te(const std::vector<int32_t> &durations);

// Splits at long gaps, decodes each packet as PWM (short high + long low is 0,
// long high + short low is 1) and reports the code most packets agree on.
// The pulse before a gap is a sync pulse, as in the EV1527 / PT2262 family,
// unless the packet's pulse count is a multiple of four, in which case it is
// the last data bit.
Analysis analyze(const std::vector<int32_t> &durations);

const char *encoding_label(Encoding encoding);

// Waveform columns for a window of the capture. Each column is a mask of the
// levels seen inside it; 0 means the column lies outside the capture.
constexpr uint8_t kColumnLow = 1;
constexpr uint8_t kColumnHigh = 2;
std::vector<uint8_t> waveform_columns(const std::vector<int32_t> &durations, uint32_t start_us, uint32_t span_us,
                                      int width);

// Zoom levels for a waveform `width` pixels wide. With a decoded code, level 0
// shows one packet at a whole number of pixels per bit, levels 1 and 2 halve
// the window each time and the last level shows the whole capture. Without a
// code the levels run from the whole capture down to one eighth of it.
constexpr int kZoomLevels = 4;
uint32_t view_span_us(const Analysis &analysis, int level, int width);
uint32_t clamp_view_start(int64_t start_us, uint32_t span_us, uint32_t total_us);

// Pixel column of a capture time inside a window; may fall outside 0..width.
int column_of(uint32_t time_us, uint32_t start_us, uint32_t span_us, int width);

// "350 us", "12.4 ms", "1.20 s".
std::string format_duration(uint32_t us);

// List label for a capture file stem: "capture-20261115-215411-433.920"
// becomes "1115 21:54:11"; a renamed capture keeps its name.
std::string capture_label(const std::string &stem);

} // namespace keys

/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Pure helpers for the RTL-SDR page: frequency maths, command building and
// output parsing, free of LVGL and process access so they can be unit-tested.
namespace sdr {

// R820T2 tuner range with the v5 direct-sampling HF path included.
constexpr uint32_t kMinHz = 100000;
constexpr uint32_t kMaxHz = 1750000000u;
// One tuner bandwidth, so rtl_power sweeps the span without retuning.
constexpr uint32_t kMaxSpanHz = 2400000u;

enum class Demod { WFM, NFM, AM };
const char *demod_label(Demod demod);

struct Preset {
    const char *label;
    uint32_t hz;
    Demod demod;
    uint32_t span_hz;
};

inline constexpr std::array<Preset, 6> kPresets = {{
    {"fm", 98000000u, Demod::WFM, 2000000u},
    {"air", 124000000u, Demod::AM, 1000000u},
    {"433", 433920000u, Demod::NFM, 2000000u},
    {"pmr", 446000000u, Demod::NFM, 1000000u},
    {"noaa", 162400000u, Demod::NFM, 1000000u},
    {"ads-b", 1090000000u, Demod::AM, 2000000u},
}};

// Tuning step sizes cycled with the step key.
inline constexpr std::array<uint32_t, 5> kSteps = {1000u, 5000u, 25000u, 100000u, 1000000u};

uint32_t clamp_hz(int64_t hz);

// "88.500" in MHz with three decimals; used for the big readout.
std::string format_mhz(uint32_t hz);
// "88.500 MHz" or, below 1 MHz, "500 kHz".
std::string format_hz(uint32_t hz);

// -1 gain means the dongle's automatic gain; otherwise whole dB.
std::string gain_label(int gain_db);

// Shell command lines run through the process API. outpath receives streamed
// output the page tails; it is quoted by the caller's directory, assumed safe.
std::string detect_cmd();
std::string power_cmd(uint32_t center_hz, uint32_t span_hz, int gain_db, const std::string &outpath);
std::string listen_cmd(uint32_t hz, Demod demod, int gain_db);
std::string decode_cmd(uint32_t hz, int gain_db, const std::string &outpath);

// rtl_test output -> is a device present, and its name if any.
struct Detect {
    bool present = false;
    std::string name;
};
Detect parse_detect(const std::string &output);

// One rtl_power CSV line -> the frequency window and power in dBm per bin.
struct PowerRow {
    bool valid = false;
    uint32_t low_hz = 0;
    uint32_t step_hz = 0;
    std::vector<int> db;
};
PowerRow parse_power_row(const std::string &line);

// Resample a power row to width columns of dBm (nearest bin), for the trace
// and waterfall.
std::vector<int> power_columns(const PowerRow &row, int width);

// dBm -> 0..levels-1 on the display scale.
int level_index(int db, int floor_db, int ceil_db, int levels);

// One rtl_433 JSON line -> "Nexus-TH  id 42 21.3C 55%" or empty if not a
// device record.
std::string summarize_433(const std::string &json);

} // namespace sdr

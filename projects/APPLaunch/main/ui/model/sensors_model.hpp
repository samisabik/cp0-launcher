/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// 433/868 MHz sensor dashboard logic for the Sensors page: rtl_433 JSON line
// parsing, a table of sensors with a temperature history each, and the text
// formatting the rows use. Free of LVGL and process access so it can be
// unit-tested.
namespace sensors {

// The two bands the page switches between.
inline constexpr std::array<uint32_t, 2> kFrequenciesHz = {433920000u, 868300000u};

constexpr int kHistorySize = 62;
// Sensors repeat each transmission several times, so a history sample is
// taken at most this often and later readings replace the newest sample.
constexpr double kHistoryIntervalS = 30.0;
constexpr std::size_t kMaxSensors = 32;

struct Reading {
    bool valid = false; // a "model" was present
    std::string model;
    std::string id;      // empty when the record has none
    std::string channel; // empty when the record has none
    bool has_temperature = false;
    double temperature_c = 0.0;
    bool has_humidity = false;
    int humidity = 0;
    int battery_ok = -1; // -1 unknown, 0 low, 1 ok
    std::string time;    // as printed by rtl_433, may be empty
};

// One line of `rtl_433 -F json` output. temperature_F is converted to Celsius.
Reading parse_line(const std::string &json);

struct Sensor {
    Reading latest;
    double first_seen = 0.0;
    double last_seen = 0.0;
    uint32_t count = 0;
    // Ring of temperatures, oldest first after history().
    std::array<float, kHistorySize> ring{};
    int ring_count = 0;
    int ring_head = 0; // index of the next write
    double last_sample = 0.0;

    std::vector<float> history() const;
};

class Table {
public:
    // Stores a reading taken at `now` (seconds on any monotonic clock).
    // Readings without temperature and humidity are ignored. Returns true
    // when the table changed.
    bool add(const Reading &reading, double now);
    bool add_line(const std::string &json, double now);
    void clear();
    // In order of first appearance, so rows keep their place on screen.
    const std::vector<Sensor> &sensors() const { return sensors_; }

private:
    std::vector<Sensor> sensors_;
};

// model + id + channel, the identity of a sensor.
std::string key(const Reading &reading);

// "12s", "3m", "2h", "4d"; negative ages read as "0s".
std::string format_age(double seconds);
// At most four characters: "21.3", " 8.7", "-2.4", "-12", "104"; "--.-"
// without a temperature.
std::string format_temperature(const Reading &reading);
// "nexus-th  id 42" or "nexus-th  id 42 ch 1", lowercase, cut to max_chars.
std::string format_name(const Reading &reading, std::size_t max_chars);
// "433.92"
std::string format_frequency(uint32_t hz);
// One line for the log file: "<stamp> <model> id <id> ch <ch> 21.3C 55% batt low".
std::string format_log_line(const Sensor &sensor, const std::string &stamp);

// Maps a history onto `width` columns and rows 0..height-1 (0 at the top),
// newest sample in the rightmost column. Columns without data are -1. The
// vertical scale covers the history's range, at least 1 degree.
std::vector<int> sparkline(const std::vector<float> &history, int width, int height);

// A line with this text is appended to the output when rtl_433 exits.
inline constexpr const char *kExitMark = "#exit";
// rtl_433 command line for a frequency: JSON lines appended to outpath,
// followed by kExitMark once the tool has exited.
std::string command(uint32_t hz, const std::string &outpath);

// The radio a reading comes from: the CC1101 on the hat, whose captured
// packets rtl_433 decodes from a pulse file, or an RTL-SDR dongle that
// rtl_433 tunes itself.
enum class Source { Cc1101 = 0, Sdr = 1 };
// "cc1101" or "sdr"
const char *source_name(Source source);
// rtl_433 command line that decodes a pulse file (.ook) of captured packets:
// JSON lines appended to outpath, followed by kExitMark.
std::string decode_command(const std::string &inpath, const std::string &outpath);

} // namespace sensors

/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Logic for the Pins page: GPIO edge lists to traces, an I2C decoder over two
// edge streams, time-per-division steps, config parsing and the I2C scan
// grid. No LVGL, no kernel headers.
namespace pins {

// A level change on one line. `level` is the level after the edge.
struct Edge {
    int64_t t_ns = 0;
    bool level = false;
};

struct LineRef {
    int chip = 0;
    int offset = 0;
};

inline constexpr std::size_t kMaxLines = 4;
inline constexpr int kMaxChip = 31;
inline constexpr int kMaxOffset = 511;

// "5,6" (offsets on chip 0) or "0:5,1:3" (chip:offset). Entries that do not
// parse and repeats are dropped; at most kMaxLines are kept.
std::vector<LineRef> parse_lines(const std::string &text);
// "sclIndex,sdaIndex": indices into the parsed line list.
bool parse_i2c_pair(const std::string &text, std::size_t line_count, int &scl, int &sda);

inline constexpr int64_t kMinDivNs = 10000;      // 10 us
inline constexpr int64_t kMaxDivNs = 1000000000; // 1 s
// Smallest 1-2-5 step that is at least `ns`.
int64_t nice_step(int64_t ns);
// The 1-2-5 neighbour of `ns` in either direction, clamped to the div range.
int64_t next_step(int64_t ns, bool larger);
std::string format_time(int64_t ns); // "10 us", "2 ms", "1 s"
std::string format_hz(uint32_t hz);  // "100 kHz"

// Edges with start_ns <= t < end_ns. `initial` is the level before the first
// stored edge; `level_at_start` receives the level just before start_ns.
std::vector<Edge> window(const std::vector<Edge> &edges, bool initial, int64_t start_ns, int64_t end_ns,
                         bool &level_at_start);
// Drops the oldest half once the list passes `max`, carrying the level over.
void trim(std::vector<Edge> &edges, bool &initial, std::size_t max);

inline constexpr uint8_t kColumnHigh = 1;
inline constexpr uint8_t kColumnEdge = 2;
// One value per pixel column over [start_ns, start_ns + span_ns): bit 0 is the
// level at the end of the column, bit 1 is set when the line changed inside it.
std::vector<uint8_t> trace_columns(const std::vector<Edge> &edges, bool initial, int64_t start_ns, int64_t span_ns,
                                   int columns);

struct I2cToken {
    enum class Kind { Start, Restart, Address, Data, Ack, Nak, Stop };
    Kind kind = Kind::Start;
    uint8_t value = 0; // 7-bit address or data byte
    bool read = false; // for Address
    int64_t t_ns = 0;
};

// Start and stop are SDA edges while SCL is high; bits are SDA sampled on SCL
// rising edges. Edges before the first start are ignored.
std::vector<I2cToken> decode_i2c(const std::vector<Edge> &scl, const std::vector<Edge> &sda, bool scl_initial,
                                 bool sda_initial);
std::string format_i2c(const std::vector<I2cToken> &tokens); // "S 48w a 0F a P"
// Median SCL rising-edge rate, 0 with fewer than three rising edges.
uint32_t i2c_clock_hz(const std::vector<Edge> &scl);

inline constexpr int kI2cFirst = 0x08;
inline constexpr int kI2cLast = 0x77;
inline constexpr int kGridColumns = 16;
inline constexpr int kGridRows = 8;

enum class Probe : uint8_t { Unknown, Empty, Found, InUse };
using ProbeMap = std::array<Probe, 128>;

// Row and column of an address in a 16-column grid. False outside the scanned range.
bool grid_cell(int address, int &row, int &column);
// Address shown in a cell, or -1 when the cell is outside the scanned range.
int grid_address(int row, int column);
int count(const ProbeMap &map, Probe state);
std::string found_list(const ProbeMap &map); // "34 48 55 6B"

} // namespace pins

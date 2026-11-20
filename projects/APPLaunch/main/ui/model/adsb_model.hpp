/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Mode S extended squitter (ADS-B) decoding for the ADSB page: frame parsing
// and CRC, callsign, airborne position (CPR global decode), velocity, and a
// table of aircraft keyed by ICAO address. Free of LVGL and process access so
// it can be unit-tested.
namespace adsb {

constexpr int kFrameBytes = 14;
using Frame = std::array<uint8_t, kFrameBytes>;

// An even and an odd position frame decode together only when received
// within this many seconds of each other.
constexpr double kCprPairSeconds = 10.0;
// Aircraft not heard for this long are dropped from the table.
constexpr double kDropSeconds = 60.0;

// One line as printed by rtl_adsb, "*8d4840d6202cc371c32ce0576098;", into
// 14 bytes. The leading '*', trailing ';' and surrounding whitespace are
// optional. False for anything that is not exactly 28 hex digits.
bool parse_frame(const std::string &line, Frame &out);

// Mode S parity over `bytes` bytes (generator 0xFFF409).
uint32_t crc24(const uint8_t *data, int bytes);
// True when the last three bytes equal the parity of the first eleven.
bool crc_ok(const Frame &frame);

int downlink_format(const Frame &frame);
uint32_t icao(const Frame &frame);
int type_code(const Frame &frame);

// Type codes 1-4. Trailing spaces removed; empty for other type codes.
std::string callsign(const Frame &frame);

// Type codes 9-18: barometric altitude and the CPR encoded position.
struct AirbornePosition {
    bool valid = false;
    bool has_altitude = false;
    int altitude_ft = 0;
    bool odd = false;
    uint32_t cpr_lat = 0; // 17 bits
    uint32_t cpr_lon = 0; // 17 bits
};
AirbornePosition airborne_position(const Frame &frame);

// Globally unambiguous position from an even and an odd frame. `odd_is_latest`
// says which of the two was received last; the result is that frame's position.
struct LatLon {
    bool valid = false;
    double lat = 0.0;
    double lon = 0.0;
};
LatLon cpr_global(uint32_t even_lat, uint32_t even_lon, uint32_t odd_lat, uint32_t odd_lon, bool odd_is_latest);

// Type code 19, subtypes 1 and 2 (ground speed, subsonic and supersonic).
struct Velocity {
    bool valid = false;
    double speed_kt = 0.0;
    double track_deg = 0.0; // clockwise from north, [0, 360)
};
Velocity velocity(const Frame &frame);

struct Aircraft {
    uint32_t icao = 0;
    std::string callsign;
    bool has_position = false;
    double lat = 0.0;
    double lon = 0.0;
    bool has_altitude = false;
    int altitude_ft = 0;
    bool has_velocity = false;
    double speed_kt = 0.0;
    double track_deg = 0.0;
    double last_seen = 0.0;

    // Latest raw CPR frame of each parity (index 0 even, 1 odd).
    bool cpr_valid[2] = {false, false};
    uint32_t cpr_lat[2] = {0, 0};
    uint32_t cpr_lon[2] = {0, 0};
    double cpr_time[2] = {0.0, 0.0};
};

class Table {
public:
    // Feeds one rtl_adsb line received at `now` (seconds on any monotonic
    // clock). Returns true when the line was a valid DF17/DF18 frame.
    bool feed(const std::string &line, double now);
    // Drops aircraft last heard more than kDropSeconds before `now`.
    void age(double now);
    void clear();
    const std::vector<Aircraft> &aircraft() const { return aircraft_; }
    const Aircraft *find(uint32_t icao) const;

private:
    std::vector<Aircraft> aircraft_;
};

// "4840D6"
std::string format_icao(uint32_t icao);
// Callsign when known, otherwise the ICAO address.
std::string label(const Aircraft &aircraft);
// Flight level, altitude in hundreds of feet as three digits ("380"); "---"
// without an altitude.
std::string format_flight_level(const Aircraft &aircraft);

} // namespace adsb

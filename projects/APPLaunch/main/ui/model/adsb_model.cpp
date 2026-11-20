/*
 * SPDX-License-Identifier: MIT
 */

#include "adsb_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace adsb {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kGenerator = 0xFFF409;
constexpr double kCprScale = 131072.0; // 2^17
constexpr int kLatZones = 15;          // NZ in the CPR definition
// A table this size holds every aircraft one receiver hears at once; the
// oldest entry makes room when it is full.
constexpr std::size_t kMaxAircraft = 128;

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

double positive_mod(double a, double b)
{
    return a - b * std::floor(a / b);
}

// Number of longitude zones at a latitude (the NL function).
int longitude_zones(double lat)
{
    const double abs_lat = std::fabs(lat);
    if (abs_lat < 1e-9) return 59;
    if (abs_lat >= 87.0) return abs_lat > 87.0 ? 1 : 2;
    const double a = 1.0 - std::cos(kPi / (2.0 * kLatZones));
    const double c = std::cos(kPi / 180.0 * abs_lat);
    return static_cast<int>(std::floor(2.0 * kPi / std::acos(1.0 - a / (c * c))));
}

} // namespace

bool parse_frame(const std::string &line, Frame &out)
{
    std::size_t begin = 0;
    std::size_t end = line.size();
    auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (begin < end && blank(line[begin])) ++begin;
    while (end > begin && blank(line[end - 1])) --end;
    if (begin < end && line[begin] == '*') ++begin;
    if (end > begin && line[end - 1] == ';') --end;
    if (end - begin != static_cast<std::size_t>(2 * kFrameBytes)) return false;
    for (int i = 0; i < kFrameBytes; ++i) {
        const int high = hex_value(line[begin + 2 * i]);
        const int low = hex_value(line[begin + 2 * i + 1]);
        if (high < 0 || low < 0) return false;
        out[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

uint32_t crc24(const uint8_t *data, int bytes)
{
    uint32_t crc = 0;
    for (int i = 0; i < bytes; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 16;
        for (int bit = 0; bit < 8; ++bit) {
            crc <<= 1;
            if (crc & 0x1000000u) crc ^= 0x1000000u | kGenerator;
        }
    }
    return crc & 0xFFFFFFu;
}

bool crc_ok(const Frame &frame)
{
    const uint32_t parity = (static_cast<uint32_t>(frame[11]) << 16) | (static_cast<uint32_t>(frame[12]) << 8) |
                            frame[13];
    return crc24(frame.data(), kFrameBytes - 3) == parity;
}

int downlink_format(const Frame &frame)
{
    return frame[0] >> 3;
}

uint32_t icao(const Frame &frame)
{
    return (static_cast<uint32_t>(frame[1]) << 16) | (static_cast<uint32_t>(frame[2]) << 8) | frame[3];
}

int type_code(const Frame &frame)
{
    return frame[4] >> 3;
}

std::string callsign(const Frame &frame)
{
    const int tc = type_code(frame);
    if (tc < 1 || tc > 4) return {};
    static const char kCharset[] = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";
    uint64_t bits = 0;
    for (int i = 5; i <= 10; ++i) bits = (bits << 8) | frame[i];
    std::string out;
    for (int i = 0; i < 8; ++i) {
        const char c = kCharset[(bits >> (42 - 6 * i)) & 0x3F];
        out += c == '#' ? ' ' : c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

AirbornePosition airborne_position(const Frame &frame)
{
    AirbornePosition position;
    const int tc = type_code(frame);
    if (tc < 9 || tc > 18) return position;
    position.valid = true;
    const uint32_t altitude = (static_cast<uint32_t>(frame[5]) << 4) | (frame[6] >> 4);
    // With the Q bit set the other eleven bits count 25 ft steps from
    // -1000 ft. Without it the field is Gillham coded in 100 ft steps, which
    // is left undecoded here.
    if (altitude != 0 && (altitude & 0x010)) {
        const uint32_t n = ((altitude & 0xFE0) >> 1) | (altitude & 0x00F);
        position.has_altitude = true;
        position.altitude_ft = static_cast<int>(n) * 25 - 1000;
    }
    position.odd = (frame[6] & 0x04) != 0;
    position.cpr_lat = ((static_cast<uint32_t>(frame[6]) & 0x03) << 15) | (static_cast<uint32_t>(frame[7]) << 7) |
                       (frame[8] >> 1);
    position.cpr_lon = ((static_cast<uint32_t>(frame[8]) & 0x01) << 16) | (static_cast<uint32_t>(frame[9]) << 8) |
                       frame[10];
    return position;
}

LatLon cpr_global(uint32_t even_lat, uint32_t even_lon, uint32_t odd_lat, uint32_t odd_lon, bool odd_is_latest)
{
    LatLon out;
    const double lat_even_frac = even_lat / kCprScale;
    const double lat_odd_frac = odd_lat / kCprScale;
    const double lon_even_frac = even_lon / kCprScale;
    const double lon_odd_frac = odd_lon / kCprScale;
    const double dlat_even = 360.0 / (4 * kLatZones);
    const double dlat_odd = 360.0 / (4 * kLatZones - 1);

    const double j = std::floor(59.0 * lat_even_frac - 60.0 * lat_odd_frac + 0.5);
    double lat_even = dlat_even * (positive_mod(j, 60.0) + lat_even_frac);
    double lat_odd = dlat_odd * (positive_mod(j, 59.0) + lat_odd_frac);
    if (lat_even >= 270.0) lat_even -= 360.0;
    if (lat_odd >= 270.0) lat_odd -= 360.0;
    if (lat_even < -90.0 || lat_even > 90.0 || lat_odd < -90.0 || lat_odd > 90.0) return out;
    // Both frames must fall in the same longitude zone count, otherwise the
    // aircraft crossed a zone boundary between them and the pair is unusable.
    if (longitude_zones(lat_even) != longitude_zones(lat_odd)) return out;

    const double lat = odd_is_latest ? lat_odd : lat_even;
    const int nl = longitude_zones(lat);
    const int zones = std::max(nl - (odd_is_latest ? 1 : 0), 1);
    const double m = std::floor(lon_even_frac * (nl - 1) - lon_odd_frac * nl + 0.5);
    double lon = (360.0 / zones) * (positive_mod(m, zones) + (odd_is_latest ? lon_odd_frac : lon_even_frac));
    if (lon >= 180.0) lon -= 360.0;
    out.valid = true;
    out.lat = lat;
    out.lon = lon;
    return out;
}

Velocity velocity(const Frame &frame)
{
    Velocity out;
    if (type_code(frame) != 19) return out;
    const int subtype = frame[4] & 0x07;
    if (subtype != 1 && subtype != 2) return out;
    const int raw_ew = ((frame[5] & 0x03) << 8) | frame[6];
    const int raw_ns = ((frame[7] & 0x7F) << 3) | (frame[8] >> 5);
    if (raw_ew == 0 || raw_ns == 0) return out;
    // Subtype 2 is the supersonic encoding with 4 kt steps.
    const int scale = subtype == 2 ? 4 : 1;
    const double east = ((frame[5] & 0x04) ? -1.0 : 1.0) * (raw_ew - 1) * scale;
    const double north = ((frame[7] & 0x80) ? -1.0 : 1.0) * (raw_ns - 1) * scale;
    out.valid = true;
    out.speed_kt = std::sqrt(east * east + north * north);
    double track = std::atan2(east, north) * 180.0 / kPi;
    if (track < 0.0) track += 360.0;
    if (track >= 360.0) track -= 360.0;
    out.track_deg = track;
    return out;
}

bool Table::feed(const std::string &line, double now)
{
    Frame frame{};
    if (!parse_frame(line, frame)) return false;
    const int df = downlink_format(frame);
    if (df != 17 && df != 18) return false;
    if (!crc_ok(frame)) return false;

    const uint32_t address = icao(frame);
    auto it = std::find_if(aircraft_.begin(), aircraft_.end(),
                           [address](const Aircraft &a) { return a.icao == address; });
    if (it == aircraft_.end()) {
        if (aircraft_.size() >= kMaxAircraft) {
            aircraft_.erase(std::min_element(aircraft_.begin(), aircraft_.end(), [](const Aircraft &a, const Aircraft &b) {
                return a.last_seen < b.last_seen;
            }));
        }
        Aircraft fresh;
        fresh.icao = address;
        aircraft_.push_back(fresh);
        it = aircraft_.end() - 1;
    }
    Aircraft &aircraft = *it;
    aircraft.last_seen = now;

    const int tc = type_code(frame);
    if (tc >= 1 && tc <= 4) {
        const std::string name = callsign(frame);
        if (!name.empty()) aircraft.callsign = name;
    } else if (tc >= 9 && tc <= 18) {
        const AirbornePosition position = airborne_position(frame);
        if (position.has_altitude) {
            aircraft.has_altitude = true;
            aircraft.altitude_ft = position.altitude_ft;
        }
        const int parity = position.odd ? 1 : 0;
        aircraft.cpr_valid[parity] = true;
        aircraft.cpr_lat[parity] = position.cpr_lat;
        aircraft.cpr_lon[parity] = position.cpr_lon;
        aircraft.cpr_time[parity] = now;
        const int other = 1 - parity;
        if (aircraft.cpr_valid[other] && now - aircraft.cpr_time[other] <= kCprPairSeconds) {
            const LatLon fix = cpr_global(aircraft.cpr_lat[0], aircraft.cpr_lon[0], aircraft.cpr_lat[1],
                                          aircraft.cpr_lon[1], position.odd);
            if (fix.valid) {
                aircraft.has_position = true;
                aircraft.lat = fix.lat;
                aircraft.lon = fix.lon;
            }
        }
    } else if (tc == 19) {
        const Velocity v = velocity(frame);
        if (v.valid) {
            aircraft.has_velocity = true;
            aircraft.speed_kt = v.speed_kt;
            aircraft.track_deg = v.track_deg;
        }
    }
    return true;
}

void Table::age(double now)
{
    aircraft_.erase(std::remove_if(aircraft_.begin(), aircraft_.end(),
                                   [now](const Aircraft &a) { return now - a.last_seen > kDropSeconds; }),
                    aircraft_.end());
}

void Table::clear()
{
    aircraft_.clear();
}

const Aircraft *Table::find(uint32_t address) const
{
    for (const Aircraft &a : aircraft_)
        if (a.icao == address) return &a;
    return nullptr;
}

std::string format_icao(uint32_t address)
{
    char text[8];
    std::snprintf(text, sizeof(text), "%06X", static_cast<unsigned>(address & 0xFFFFFFu));
    return text;
}

std::string label(const Aircraft &aircraft)
{
    return aircraft.callsign.empty() ? format_icao(aircraft.icao) : aircraft.callsign;
}

std::string format_flight_level(const Aircraft &aircraft)
{
    if (!aircraft.has_altitude) return "---";
    const int level = std::clamp((aircraft.altitude_ft + 50) / 100, 0, 999);
    char text[8];
    std::snprintf(text, sizeof(text), "%03d", level);
    return text;
}

} // namespace adsb

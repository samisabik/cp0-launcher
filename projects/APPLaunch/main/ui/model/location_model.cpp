/*
 * SPDX-License-Identifier: MIT
 */

#include "location_model.hpp"

#include <cmath>
#include <cstdlib>

namespace location {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusKm = 6371.0088;
constexpr double kKmPerNm = 1.852;
constexpr double kLightSpeed = 299792458.0;
constexpr double kDipoleShortening = 0.95;

double rad(double deg)
{
    return deg * kPi / 180.0;
}

// Parses one number that must fill [begin, end) apart from blanks.
bool parse_number(const std::string &text, std::size_t begin, std::size_t end, double &out)
{
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' ||
                           text[end - 1] == '\n'))
        --end;
    if (begin >= end) return false;
    const std::string token = text.substr(begin, end - begin);
    for (char c : token)
        if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+')) return false;
    char *stop = nullptr;
    out = std::strtod(token.c_str(), &stop);
    return stop && *stop == '\0' && std::isfinite(out);
}

} // namespace

Position parse(const std::string &text)
{
    Position position;
    const std::size_t line_end = text.find('\n');
    const std::string line = text.substr(0, line_end);
    const std::size_t comma = line.find(',');
    if (comma == std::string::npos) return position;
    double lat = 0.0;
    double lon = 0.0;
    if (!parse_number(line, 0, comma, lat) || !parse_number(line, comma + 1, line.size(), lon)) return position;
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return position;
    position.valid = true;
    position.lat = lat;
    position.lon = lon;
    return position;
}

std::string file_path(const std::string &home)
{
    return home + "/.config/cp0/location";
}

const char *set_hint()
{
    return "echo \"lat,lon\" > \\\n~/.config/cp0/location";
}

double distance_km(const Position &from, const Position &to)
{
    const double dlat = rad(to.lat - from.lat);
    const double dlon = rad(to.lon - from.lon);
    const double s1 = std::sin(dlat / 2.0);
    const double s2 = std::sin(dlon / 2.0);
    const double h = s1 * s1 + std::cos(rad(from.lat)) * std::cos(rad(to.lat)) * s2 * s2;
    return 2.0 * kEarthRadiusKm * std::asin(std::sqrt(h < 1.0 ? h : 1.0));
}

double distance_nm(const Position &from, const Position &to)
{
    return distance_km(from, to) / kKmPerNm;
}

double bearing_deg(const Position &from, const Position &to)
{
    const double dlon = rad(to.lon - from.lon);
    const double y = std::sin(dlon) * std::cos(rad(to.lat));
    const double x = std::cos(rad(from.lat)) * std::sin(rad(to.lat)) -
                     std::sin(rad(from.lat)) * std::cos(rad(to.lat)) * std::cos(dlon);
    double bearing = std::atan2(y, x) * 180.0 / kPi;
    if (bearing < 0.0) bearing += 360.0;
    if (bearing >= 360.0) bearing -= 360.0;
    return bearing;
}

double quarter_wave_cm(double hz)
{
    if (!(hz > 0.0)) return 0.0;
    return kLightSpeed / hz / 4.0 * 100.0;
}

double dipole_leg_cm(double hz)
{
    return quarter_wave_cm(hz) * kDipoleShortening;
}

} // namespace location

/*
 * SPDX-License-Identifier: MIT
 */

#include "gps_model.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace gps {

namespace {

constexpr double kKnotKmh = 1.852;

std::vector<std::string> split(const std::string &text, char separator)
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find(separator, start);
        if (end == std::string::npos) {
            fields.push_back(text.substr(start));
            return fields;
        }
        fields.push_back(text.substr(start, end - start));
        start = end + 1;
    }
}

double number(const std::string &text)
{
    return text.empty() ? 0.0 : std::strtod(text.c_str(), nullptr);
}

unsigned checksum(const std::string &text, std::size_t first, std::size_t last)
{
    unsigned sum = 0;
    for (std::size_t i = first; i < last; ++i) sum ^= static_cast<unsigned char>(text[i]);
    return sum;
}

// RMC "ddmmyy" as "20yy-mm-dd".
std::string format_date(const std::string &nmea_date)
{
    if (nmea_date.size() != 6) return {};
    return "20" + nmea_date.substr(4, 2) + "-" + nmea_date.substr(2, 2) + "-" + nmea_date.substr(0, 2);
}

std::string fixed(double value, int decimals)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
}

} // namespace

std::string command(const std::string &body)
{
    char tail[8];
    std::snprintf(tail, sizeof(tail), "*%02X\r\n", checksum(body, 0, body.size()));
    return "$" + body + tail;
}

std::string rate_command(int interval_ms)
{
    return command("PCAS02," + std::to_string(interval_ms));
}

bool checksum_ok(const std::string &sentence)
{
    if (sentence.size() < 4 || sentence[0] != '$') return false;
    const std::size_t star = sentence.size() - 3;
    if (sentence[star] != '*') return false;
    const unsigned char high = static_cast<unsigned char>(sentence[star + 1]);
    const unsigned char low = static_cast<unsigned char>(sentence[star + 2]);
    if (!std::isxdigit(high) || !std::isxdigit(low)) return false;
    return std::strtoul(sentence.c_str() + star + 1, nullptr, 16) == checksum(sentence, 1, star);
}

double coordinate(const std::string &value, const std::string &hemisphere)
{
    const double raw = number(value);
    const double degrees = std::floor(raw / 100.0);
    const double result = degrees + (raw - degrees * 100.0) / 60.0;
    return hemisphere == "S" || hemisphere == "W" ? -result : result;
}

std::string format_time(const std::string &nmea_time)
{
    if (nmea_time.size() < 6) return nmea_time;
    return nmea_time.substr(0, 2) + ":" + nmea_time.substr(2, 2) + ":" + nmea_time.substr(4);
}

std::string csv_row(const Fix &fix)
{
    std::string row = fix.date.empty() ? fix.time : fix.date + "T" + fix.time + "Z";
    row += "," + fixed(fix.lat, 7) + "," + fixed(fix.lon, 7) + "," + fixed(fix.alt_m, 1) + ",";
    if (fix.has_motion) row += fixed(fix.speed_kmh, 2);
    row += ",";
    if (fix.has_motion) row += fixed(fix.course_deg, 1);
    row += "," + std::to_string(fix.sats) + "," + fixed(fix.hdop, 1) + "," + std::to_string(fix.quality) + "\n";
    return row;
}

std::string log_name(int year, int month, int day, int hour, int minute, int second)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%04d%02d%02d-%02d%02d%02d.csv", year, month, day, hour, minute, second);
    return text;
}

std::vector<Fix> Parser::feed(const char *data, std::size_t size)
{
    std::vector<Fix> fixes;
    for (std::size_t i = 0; i < size; ++i) {
        const char c = data[i];
        if (c == '\n') {
            if (!line_.empty() && line_.back() == '\r') line_.pop_back();
            Fix fix;
            if (take(line_, fix)) fixes.push_back(fix);
            line_.clear();
        } else if (c == '$') {
            line_.assign(1, c);
        } else if (line_.size() < kMaxSentence) {
            line_.push_back(c);
        }
    }
    return fixes;
}

int Parser::in_view() const
{
    int total = 0;
    for (const auto &entry : in_view_) total += entry.second;
    return total;
}

bool Parser::take(const std::string &sentence, Fix &fix)
{
    if (!checksum_ok(sentence)) return false;
    const std::vector<std::string> f = split(sentence.substr(1, sentence.size() - 4), ',');
    if (f[0].size() != 5) return false;
    const std::string talker = f[0].substr(0, 2);
    const std::string type = f[0].substr(2);

    if (type == "RMC" && f.size() >= 10) {
        rmc_time_ = f[1];
        rmc_has_motion_ = f[2] == "A" && !f[7].empty();
        rmc_speed_kmh_ = number(f[7]) * kKnotKmh;
        rmc_course_deg_ = number(f[8]);
        const std::string date = format_date(f[9]);
        if (!date.empty()) date_ = date;
        return false;
    }
    if (type == "GSV" && f.size() >= 4) {
        in_view_[talker] = std::atoi(f[3].c_str());
        return false;
    }
    if (type != "GGA" || f.size() < 10) return false;

    fix.quality = std::atoi(f[6].c_str());
    fix.valid = fix.quality > 0 && !f[2].empty() && !f[4].empty();
    fix.date = date_;
    fix.time = format_time(f[1]);
    fix.lat = coordinate(f[2], f[3]);
    fix.lon = coordinate(f[4], f[5]);
    fix.sats = std::atoi(f[7].c_str());
    fix.hdop = number(f[8]);
    fix.alt_m = number(f[9]);
    if (rmc_has_motion_ && rmc_time_ == f[1]) {
        fix.has_motion = true;
        fix.speed_kmh = rmc_speed_kmh_;
        fix.course_deg = rmc_course_deg_;
    }
    return true;
}

} // namespace gps

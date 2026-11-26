/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

// Logic of the GPS page: NMEA 0183 sentences from the receiver in, position
// fixes and CSV log rows out, and the commands that set the receiver up.
namespace gps {

constexpr const char *kCsvHeader = "utc,lat,lon,alt_m,speed_kmh,course_deg,sats,hdop,quality\n";

// One position report, built from a GGA sentence and the RMC sentence of the
// same instant.
struct Fix {
    bool valid = false;      // the receiver has a position
    std::string date;        // "2026-11-26" (UTC), empty until the receiver knows it
    std::string time;        // "23:09:18.10" (UTC)
    double lat = 0;          // degrees, north positive
    double lon = 0;          // degrees, east positive
    double alt_m = 0;        // above mean sea level
    bool has_motion = false; // speed_kmh and course_deg are set
    double speed_kmh = 0;
    double course_deg = 0;   // over ground, from true north
    int sats = 0;            // satellites used in the position
    double hdop = 0;
    int quality = 0;         // GGA fix quality, 0 for no position
};

// "$<body>*<checksum>\r\n", the framing of every NMEA sentence.
std::string command(const std::string &body);

// CASIC command (AT6558, AT6668 receivers) that sets the time between
// position reports.
std::string rate_command(int interval_ms);

// True for a whole sentence, without line ending, whose checksum matches.
bool checksum_ok(const std::string &sentence);

// NMEA "ddmm.mmmm" with its hemisphere letter, in signed degrees.
double coordinate(const std::string &value, const std::string &hemisphere);

// "hhmmss.ss" as "hh:mm:ss.ss"; anything shorter is returned unchanged.
std::string format_time(const std::string &nmea_time);

// One line of the log, ending in a newline, in the column order of kCsvHeader.
std::string csv_row(const Fix &fix);

// "20261126-230918.csv"
std::string log_name(int year, int month, int day, int hour, int minute, int second);

// Sentence assembler. Bytes arrive in arbitrary chunks; each GGA sentence
// with a good checksum yields one Fix, valid or not.
class Parser
{
public:
    std::vector<Fix> feed(const char *data, std::size_t size);

    // Satellites the receiver can see, summed over the constellations.
    int in_view() const;

private:
    static constexpr std::size_t kMaxSentence = 120;

    std::string line_;
    std::string rmc_time_;
    std::string date_;
    bool rmc_has_motion_ = false;
    double rmc_speed_kmh_ = 0;
    double rmc_course_deg_ = 0;
    std::map<std::string, int> in_view_;

    bool take(const std::string &sentence, Fix &fix);
};

} // namespace gps

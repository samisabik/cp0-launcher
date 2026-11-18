/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Satellite pass prediction for the Sat page: TLE parsing, a simplified
// orbit propagator, look angles for an observer and a pass search. Free of
// LVGL, file and process access so it can be unit-tested.
//
// The propagator is NOT SGP4. It moves the TLE mean elements with the J2
// secular rates of the ascending node, argument of perigee and mean anomaly,
// adds the TLE's first mean-motion derivative, solves Kepler's equation and
// rotates the result to the observer's sky. Drag beyond that term, the J2
// periodic terms, deep-space effects and the TEME frame details are left
// out. Its accuracy against SGP4 is unverified. It is meant for pass times
// of near-circular low-orbit satellites from TLEs a few days old at most.
namespace sat {

struct Tle {
    std::string name;
    int satnum = 0;
    double epoch_unix = 0.0;        // seconds since 1970-01-01 UTC
    double ndot2_rev_day2 = 0.0;    // first derivative of mean motion / 2
    double inclination_deg = 0.0;
    double raan_deg = 0.0;
    double eccentricity = 0.0;
    double arg_perigee_deg = 0.0;
    double mean_anomaly_deg = 0.0;
    double mean_motion_rev_day = 0.0;
};

// Modulo-10 checksum of the first 68 columns: digits add their value, '-'
// adds one.
int tle_checksum(const std::string &line);
// Parses the two element lines. Both must be at least 69 columns, start with
// "1 " and "2 ", carry the same catalogue number and pass the checksum.
bool parse_tle(const std::string &name, const std::string &line1, const std::string &line2, Tle &out);
// Parses a file of 3-line element sets (name, line 1, line 2). A set without
// a name line is accepted and named by its catalogue number. Bad sets are
// skipped; duplicates of a catalogue number keep the first.
std::vector<Tle> parse_tle_file(const std::string &text);

double period_minutes(const Tle &tle);
// Semi-major axis from the mean motion, corrected for J2 the way the TLE
// mean motion is defined (Kozai to Brouwer).
double semi_major_axis_km(const Tle &tle);

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// Earth-centred inertial position at a Unix time.
Vec3 position_eci_km(const Tle &tle, double unix_time);
// Greenwich mean sidereal time in radians, [0, 2 pi).
double gmst_rad(double unix_time);

struct Observer {
    double lat_deg = 0.0;
    double lon_deg = 0.0;
    double alt_m = 0.0;
};

struct Look {
    double azimuth_deg = 0.0;   // clockwise from north, [0, 360)
    double elevation_deg = 0.0; // [-90, 90]
    double range_km = 0.0;
};
Look look_angles(const Tle &tle, const Observer &observer, double unix_time);

struct Pass {
    double aos = 0.0; // rises above the horizon, Unix time
    double los = 0.0; // sets below the horizon
    double tmax = 0.0;
    double max_elevation_deg = 0.0;
};

constexpr double kMinPassElevationDeg = 10.0;

// Passes that begin within `hours` of `start` and peak at or above
// `min_elevation_deg`, in time order. AOS and LOS are horizon crossings; a
// pass already in progress at `start` has AOS equal to `start`.
std::vector<Pass> find_passes(const Tle &tle, const Observer &observer, double start, double hours = 24.0,
                              double min_elevation_deg = kMinPassElevationDeg);

// Built-in downlink for a recognised satellite name, or 0.
uint32_t downlink_hz(const std::string &name);
// Antenna hint frequency when the selected satellite has no known downlink.
constexpr uint32_t kDefaultAntennaHz = 137100000u;

// One name substring per line; blank lines and lines starting with '#' are
// skipped.
std::vector<std::string> parse_watch(const std::string &text);
// True for the built-in interest list (the ISS and METEOR satellites) and
// for names containing any watch substring, compared without case.
bool interesting(const std::string &name, const std::vector<std::string> &watch);

// Lowercase name for a narrow column: "ISS (ZARYA)" -> "iss",
// "METEOR-M2 3" -> "meteorm23".
std::string short_name(const std::string &name, std::size_t max_chars);
// "04:12" below an hour, "1:04:12" above; negative reads "00:00".
std::string format_countdown(double seconds);
// Minutes rounded to nearest, "14m"; at least "1m".
std::string format_duration(double seconds);
// "12s", "3m", "5h", "2d"
std::string format_age(double seconds);
// "145.800"
std::string format_mhz(uint32_t hz);

// 44-byte header for mono signed 16-bit PCM.
std::vector<uint8_t> wav_header(uint32_t sample_rate, uint32_t data_bytes);
constexpr uint32_t kRecordRate = 48000;
// "<short name>-<stamp>.wav" with only [a-z0-9] kept from the name.
std::string record_file_name(const std::string &name, const std::string &stamp);
// rtl_fm narrow FM at kRecordRate, raw samples appended to `path`.
std::string record_command(uint32_t hz, const std::string &path);
// Fetches Celestrak's stations and weather groups into `path` through a
// temporary file, then writes "ok" or "fail" to `status_path`.
std::string update_command(const std::string &path, const std::string &status_path);

} // namespace sat

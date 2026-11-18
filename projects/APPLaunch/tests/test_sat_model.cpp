// SPDX-License-Identifier: MIT

#include "../main/ui/model/sat_model.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

using namespace sat;

// Element sets written for these tests in the standard fixed-column format
// (ISS-like and METEOR-like orbits), with valid checksums.
static const char *kIssName = "ISS (ZARYA)";
static const char *kIss1 = "1 25544U 98067A   26320.50000000  .00012345  00000-0  22222-3 0  9996";
static const char *kIss2 = "2 25544  51.6400 120.0000 0005000  90.0000 270.0000 15.50000000123450";
static const char *kMeteor1 = "1 57166U 23091A   26320.25000000  .00000150  00000-0  88888-4 0  9990";
static const char *kMeteor2 = "2 57166  98.7500  15.0000 0003000 100.0000 260.0000 14.24000000176548";

constexpr double kPi = 3.14159265358979323846;
// 2026-11-16 12:00:00 UTC, day 320.5 of 2026.
constexpr double kIssEpoch = 1794830400.0;

static bool near(double a, double b, double tolerance)
{
    return std::fabs(a - b) <= tolerance;
}

static Tle iss()
{
    Tle tle;
    const bool ok = parse_tle(kIssName, kIss1, kIss2, tle);
    assert(ok);
    return tle;
}

static void test_parse()
{
    assert(tle_checksum(kIss1) == 6 && tle_checksum(kIss2) == 0);
    const Tle tle = iss();
    assert(tle.name == "ISS (ZARYA)");
    assert(tle.satnum == 25544);
    assert(near(tle.epoch_unix, kIssEpoch, 1e-3));
    assert(near(tle.ndot2_rev_day2, 0.00012345, 1e-12));
    assert(near(tle.inclination_deg, 51.64, 1e-9));
    assert(near(tle.raan_deg, 120.0, 1e-9));
    assert(near(tle.eccentricity, 0.0005, 1e-12));
    assert(near(tle.arg_perigee_deg, 90.0, 1e-9));
    assert(near(tle.mean_anomaly_deg, 270.0, 1e-9));
    assert(near(tle.mean_motion_rev_day, 15.5, 1e-9));

    Tle bad;
    // Wrong checksum digit, swapped lines, mismatched catalogue numbers, short lines.
    std::string broken = kIss1;
    broken[68] = '7';
    assert(!parse_tle(kIssName, broken, kIss2, bad));
    broken = kIss2;
    broken[10] = '2'; // inclination changed without fixing the checksum
    assert(!parse_tle(kIssName, kIss1, broken, bad));
    assert(!parse_tle(kIssName, kIss2, kIss1, bad));
    assert(!parse_tle(kIssName, kIss1, kMeteor2, bad));
    assert(!parse_tle(kIssName, "1 25544U", "2 25544", bad));
    assert(!parse_tle(kIssName, "", "", bad));

    const std::string file = std::string("ISS (ZARYA)             \r\n") + kIss1 + "\r\n" + kIss2 + "\r\n" +
                             "METEOR-M2 3\n" + kMeteor1 + "\n" + kMeteor2 + "\n" + "BROKEN SAT\n" + kIss1 + "\n" +
                             "garbage line\n" + "ISS DUPLICATE\n" + kIss1 + "\n" + kIss2 + "\n";
    const std::vector<Tle> all = parse_tle_file(file);
    assert(all.size() == 2);
    assert(all[0].name == "ISS (ZARYA)" && all[0].satnum == 25544);
    assert(all[1].name == "METEOR-M2 3" && all[1].satnum == 57166);
    assert(near(all[1].inclination_deg, 98.75, 1e-9) && near(all[1].mean_motion_rev_day, 14.24, 1e-9));
    // 2026 day 320.25 is six hours before the ISS epoch above.
    assert(near(all[1].epoch_unix, kIssEpoch - 6 * 3600.0, 1e-3));

    // Two-line sets without a name take the catalogue number.
    const std::vector<Tle> bare = parse_tle_file(std::string(kIss1) + "\n" + kIss2 + "\n");
    assert(bare.size() == 1 && bare[0].name == "25544");
    assert(parse_tle_file("").empty());
    assert(parse_tle_file("no elements here\nat all\n").empty());
}

static void test_orbit()
{
    const Tle tle = iss();
    assert(near(period_minutes(tle), 1440.0 / 15.5, 1e-9));
    assert(near(period_minutes(tle), 92.9, 0.01));
    const double a = semi_major_axis_km(tle);
    assert(a > 6760.0 && a < 6810.0);

    // Radius stays near the semi-major axis around the orbit, and the
    // position returns to nearly the same radius one period later.
    for (double t = 0.0; t < 6000.0; t += 60.0) {
        const Vec3 p = position_eci_km(tle, kIssEpoch + t);
        const double r = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        assert(r > a * (1.0 - 0.0006) && r < a * (1.0 + 0.0006));
        assert(r > 6700.0 && r < 6850.0);
        // Latitude never exceeds the inclination.
        assert(std::fabs(std::asin(p.z / r)) <= 51.64 * kPi / 180.0 + 1e-6);
    }
    // Speed from two positions one second apart: about 7.66 km/s for this orbit.
    const Vec3 p0 = position_eci_km(tle, kIssEpoch);
    const Vec3 p1 = position_eci_km(tle, kIssEpoch + 1.0);
    const double speed = std::sqrt(std::pow(p1.x - p0.x, 2) + std::pow(p1.y - p0.y, 2) + std::pow(p1.z - p0.z, 2));
    assert(near(speed, std::sqrt(398600.4418 / a), 0.02));
    // At the epoch: argument of latitude 90 + 270 = 0, so the satellite sits
    // on the ascending node, in the equatorial plane at RAAN 120 degrees.
    assert(std::fabs(p0.z) < 10.0);
    assert(near(std::atan2(p0.y, p0.x) * 180.0 / kPi, 120.0, 0.1));
    assert(p1.z > p0.z);

    // GMST at J2000.0 (2000-01-01 12:00 UT) is 18h 41m 50.548s.
    assert(near(gmst_rad(946728000.0) * 180.0 / kPi, 280.46062, 1e-4));
    // One sidereal day later it is back at the same angle.
    assert(near(gmst_rad(946728000.0 + 86164.0905) * 180.0 / kPi, 280.46062, 1e-3));
}

static void test_look_angles()
{
    const Tle tle = iss();
    const Observer observer{52.37, 4.90, 0.0};
    double lowest = 90.0;
    double highest = -90.0;
    for (double t = 0.0; t < 86400.0; t += 20.0) {
        const Look look = look_angles(tle, observer, kIssEpoch + t);
        assert(look.elevation_deg >= -90.0 && look.elevation_deg <= 90.0);
        assert(look.azimuth_deg >= 0.0 && look.azimuth_deg < 360.0);
        assert(look.range_km > 300.0 && look.range_km < 14000.0);
        lowest = std::min(lowest, look.elevation_deg);
        highest = std::max(highest, look.elevation_deg);
    }
    assert(lowest < -45.0 && highest > 10.0);

    // An observer at the sub-satellite point sees it almost overhead, about
    // one orbit height away.
    for (double t : {0.0, 777.0, 3000.0, 40000.0}) {
        const double when = kIssEpoch + t;
        const Vec3 p = position_eci_km(tle, when);
        const double r = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        Observer below;
        below.lat_deg = std::asin(p.z / r) * 180.0 / kPi;
        below.lon_deg = (std::atan2(p.y, p.x) - gmst_rad(when)) * 180.0 / kPi;
        const Look look = look_angles(tle, below, when);
        assert(look.elevation_deg > 85.0);
        assert(look.range_km > 380.0 && look.range_km < 450.0);
        // From 20 degrees further north the satellite is to the south.
        if (below.lat_deg < 60.0) {
            Observer north = below;
            north.lat_deg += 20.0;
            const Look south = look_angles(tle, north, when);
            assert(south.azimuth_deg > 170.0 && south.azimuth_deg < 190.0);
            assert(south.elevation_deg < 20.0);
        }
        // From further east it is to the west.
        Observer east = below;
        east.lon_deg += 10.0;
        const Look west = look_angles(tle, east, when);
        assert(west.azimuth_deg > 225.0 && west.azimuth_deg < 315.0);
    }
}

static void test_passes()
{
    const Tle tle = iss();
    const Observer observer{45.0, 9.0, 100.0};
    const std::vector<Pass> passes = find_passes(tle, observer, kIssEpoch, 24.0);
    assert(!passes.empty());
    // An ISS-like orbit gives a mid-latitude site a handful of passes a day.
    assert(passes.size() >= 2 && passes.size() <= 8);
    double previous_los = kIssEpoch;
    for (const Pass &pass : passes) {
        assert(pass.aos < pass.tmax && pass.tmax < pass.los);
        assert(pass.aos >= previous_los);
        assert(pass.aos >= kIssEpoch && pass.aos <= kIssEpoch + 24 * 3600.0);
        assert(pass.max_elevation_deg >= kMinPassElevationDeg && pass.max_elevation_deg <= 90.0);
        const double duration = pass.los - pass.aos;
        assert(duration > 240.0 && duration < 720.0);
        // AOS and LOS are on the horizon, and the peak is the peak.
        assert(std::fabs(look_angles(tle, observer, pass.aos).elevation_deg) < 0.1);
        assert(std::fabs(look_angles(tle, observer, pass.los).elevation_deg) < 0.1);
        for (double t = pass.aos; t <= pass.los; t += 5.0)
            assert(look_angles(tle, observer, t).elevation_deg <= pass.max_elevation_deg + 0.01);
        previous_los = pass.los;
    }
    // A higher threshold returns a subset.
    const std::vector<Pass> high = find_passes(tle, observer, kIssEpoch, 24.0, 45.0);
    assert(high.size() <= passes.size());
    for (const Pass &pass : high) assert(pass.max_elevation_deg >= 45.0);
    // Starting in the middle of a pass reports it with AOS at the start.
    const double inside = passes.front().tmax;
    const std::vector<Pass> running = find_passes(tle, observer, inside, 1.0, 0.0);
    assert(!running.empty() && running.front().aos == inside && near(running.front().los, passes.front().los, 2.0));
    // An orbit of 51.6 degrees never rises 10 degrees above the pole's horizon.
    assert(find_passes(tle, Observer{89.0, 0.0, 0.0}, kIssEpoch, 24.0).empty());
    assert(find_passes(tle, observer, kIssEpoch, 0.0).empty());
}

static void test_names()
{
    assert(downlink_hz("ISS (ZARYA)") == 145800000u);
    assert(downlink_hz("iss") == 145800000u);
    assert(downlink_hz("METEOR-M2 3") == 137900000u);
    assert(downlink_hz("METEOR-M2 4") == 137900000u);
    assert(downlink_hz("METEOR-M2 2") == 0);
    assert(downlink_hz("ISS DEB") == 0);
    assert(downlink_hz("NOAA 19") == 0);
    assert(downlink_hz("") == 0);

    const std::vector<std::string> watch = parse_watch("# my list\n\n  noaa 2\r\nTIANHE\n");
    assert(watch.size() == 2 && watch[0] == "noaa 2" && watch[1] == "TIANHE");
    assert(interesting("ISS (ZARYA)", {}));
    assert(interesting("METEOR-M2 4", {}));
    assert(!interesting("ISS (NAUKA)", {}));
    assert(!interesting("ISS DEB", {}));
    assert(!interesting("NOAA 20", {}));
    assert(interesting("NOAA 20", watch));
    assert(interesting("CSS (TIANHE)", watch));
    assert(!interesting("NOAA 19", watch));

    assert(short_name("ISS (ZARYA)", 9) == "iss");
    assert(short_name("METEOR-M2 3", 9) == "meteorm23");
    assert(short_name("METEOR-M2 4", 9) == "meteorm24");
    assert(short_name("NOAA 20", 9) == "noaa 20");
    assert(short_name("FENGYUN 3D LONGNAME 7", 9) == "fengyume7");
    assert(short_name("ABCDEFGHIJKL", 3) == "abc");

    assert(format_countdown(252.0) == "04:12");
    assert(format_countdown(-3.0) == "00:00");
    assert(format_countdown(3599.9) == "59:59");
    assert(format_countdown(3852.0) == "1:04:12");
    assert(format_duration(14 * 60.0 + 10.0) == "14m");
    assert(format_duration(5.0) == "1m");
    assert(format_age(30.0) == "30s" && format_age(200.0) == "3m" && format_age(5 * 3600.0) == "5h" &&
           format_age(3 * 86400.0) == "3d");
    assert(format_mhz(145800000u) == "145.800" && format_mhz(137900000u) == "137.900");
}

static void test_recording()
{
    const std::vector<uint8_t> header = wav_header(48000, 96000);
    assert(header.size() == 44);
    assert(std::string(header.begin(), header.begin() + 4) == "RIFF");
    assert(std::string(header.begin() + 8, header.begin() + 16) == "WAVEfmt ");
    assert(std::string(header.begin() + 36, header.begin() + 40) == "data");
    auto u32 = [&](int at) {
        return static_cast<uint32_t>(header[at]) | (static_cast<uint32_t>(header[at + 1]) << 8) |
               (static_cast<uint32_t>(header[at + 2]) << 16) | (static_cast<uint32_t>(header[at + 3]) << 24);
    };
    assert(u32(4) == 36 + 96000 && u32(16) == 16 && u32(24) == 48000 && u32(28) == 96000 && u32(40) == 96000);
    assert(header[20] == 1 && header[22] == 1 && header[32] == 2 && header[34] == 16);

    assert(record_file_name("ISS (ZARYA)", "20261118-214200") == "iss-20261118-214200.wav");
    assert(record_file_name("METEOR-M2 3", "x") == "meteorm23-x.wav");
    assert(record_file_name("'; rm -rf /", "x") == "rmrf-x.wav");
    assert(record_file_name("", "x") == "sat-x.wav");
    assert(record_command(145800000u, "/home/pi/sat/a.wav") ==
           "rtl_fm -f 145800000 -M fm -s 48000 - >> '/home/pi/sat/a.wav' 2>/dev/null");
    const std::string update = update_command("/home/pi/sat/tle.txt", "/run/st");
    assert(update.find("curl -fsSL") != std::string::npos);
    assert(update.find("'https://celestrak.org/NORAD/elements/gp.php?GROUP=stations&FORMAT=tle'") != std::string::npos);
    assert(update.find("'https://celestrak.org/NORAD/elements/gp.php?GROUP=weather&FORMAT=tle'") != std::string::npos);
    assert(update.find("mv '/home/pi/sat/tle.txt.part' '/home/pi/sat/tle.txt'") != std::string::npos);
}

int main()
{
    test_parse();
    test_orbit();
    test_look_angles();
    test_passes();
    test_names();
    test_recording();
    std::puts("sat_model: ok");
    return 0;
}

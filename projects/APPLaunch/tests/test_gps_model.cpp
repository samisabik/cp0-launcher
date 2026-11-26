/*
 * SPDX-License-Identifier: MIT
 */

#include "../main/ui/model/gps_model.hpp"

#include <cassert>
#include <cmath>
#include <string>
#include <vector>

namespace {

bool near(double a, double b, double tolerance = 1e-6)
{
    return std::fabs(a - b) < tolerance;
}

std::vector<gps::Fix> feed(gps::Parser &parser, const std::string &text)
{
    return parser.feed(text.data(), text.size());
}

void commands()
{
    assert(gps::command("PCAS02,100") == "$PCAS02,100*1E\r\n");
    assert(gps::rate_command(1000) == "$PCAS02,1000*2E\r\n");
}

void checksums()
{
    assert(gps::checksum_ok("$GNRMC,,V,,,,,,,,,,N,V*37"));
    assert(gps::checksum_ok("$GPGSV,1,1,00,1*64"));
    assert(!gps::checksum_ok("$GPGSV,1,1,00,1*65"));
    assert(!gps::checksum_ok("$GPGSV,1,1,00,1"));
    assert(!gps::checksum_ok("GPGSV,1,1,00,1*64"));
    assert(!gps::checksum_ok("$GPGSV,1,1,00,1*6G"));
    assert(!gps::checksum_ok(""));
}

void coordinates()
{
    assert(near(gps::coordinate("4807.038", "N"), 48.1173));
    assert(near(gps::coordinate("4807.038", "S"), -48.1173));
    assert(near(gps::coordinate("01131.000", "E"), 11.5166667));
    assert(near(gps::coordinate("00003.000", "W"), -0.05));
    assert(near(gps::coordinate("", ""), 0.0));
    assert(gps::format_time("230918.10") == "23:09:18.10");
    assert(gps::format_time("") == "");
}

void fixes()
{
    gps::Parser parser;
    // No position yet: the GGA still reports, as an invalid fix.
    std::vector<gps::Fix> out = feed(parser, gps::command("GNRMC,,V,,,,,,,,,,N,V") + gps::command("GNGGA,,,,,,0,00,25.5,,,,,,"));
    assert(out.size() == 1);
    assert(!out[0].valid);
    assert(out[0].date.empty());

    // A sentence split across two reads, with the RMC of the same instant.
    const std::string rmc = gps::command("GNRMC,123519.10,A,4807.038,N,01131.000,E,10.0,084.4,230394,,,A,V");
    const std::string gga = gps::command("GNGGA,123519.10,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
    assert(feed(parser, rmc + gga.substr(0, 20)).empty());
    out = feed(parser, gga.substr(20));
    assert(out.size() == 1);
    const gps::Fix &fix = out[0];
    assert(fix.valid);
    assert(fix.date == "2094-03-23");
    assert(fix.time == "12:35:19.10");
    assert(near(fix.lat, 48.1173));
    assert(near(fix.lon, 11.5166667));
    assert(near(fix.alt_m, 545.4));
    assert(fix.sats == 8);
    assert(near(fix.hdop, 0.9));
    assert(fix.quality == 1);
    assert(fix.has_motion);
    assert(near(fix.speed_kmh, 18.52));
    assert(near(fix.course_deg, 84.4));
    assert(gps::csv_row(fix) == "2094-03-23T12:35:19.10Z,48.1173000,11.5166667,545.4,18.52,84.4,8,0.9,1\n");

    // A GGA whose RMC belongs to another instant carries no speed or course.
    out = feed(parser, gps::command("GNGGA,123519.20,4807.038,S,01131.000,W,1,08,0.9,545.4,M,46.9,M,,"));
    assert(out.size() == 1);
    assert(!out[0].has_motion);
    assert(near(out[0].lat, -48.1173));
    assert(near(out[0].lon, -11.5166667));
    assert(gps::csv_row(out[0]) == "2094-03-23T12:35:19.20Z,-48.1173000,-11.5166667,545.4,,,8,0.9,1\n");

    // Corrupt and over-long input is dropped and the parser recovers.
    assert(feed(parser, "$GNGGA,123519.30,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*00\r\n").empty());
    assert(feed(parser, std::string(500, 'x') + "\r\n").empty());
    assert(feed(parser, "garbage" + gga).size() == 1);
}

void satellites()
{
    gps::Parser parser;
    assert(parser.in_view() == 0);
    feed(parser, gps::command("GPGSV,2,1,07,01,40,083,46") + gps::command("BDGSV,1,1,05,1"));
    assert(parser.in_view() == 12);
    feed(parser, gps::command("GPGSV,1,1,00,1"));
    assert(parser.in_view() == 5);
}

void names()
{
    assert(gps::log_name(2026, 11, 26, 23, 9, 18) == "20261126-230918.csv");
    assert(std::string(gps::kCsvHeader) == "utc,lat,lon,alt_m,speed_kmh,course_deg,sats,hdop,quality\n");
}

} // namespace

int main()
{
    commands();
    checksums();
    coordinates();
    fixes();
    satellites();
    names();
    return 0;
}

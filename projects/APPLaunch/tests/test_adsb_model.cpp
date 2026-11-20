// SPDX-License-Identifier: MIT

#include "../main/ui/model/adsb_model.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

using namespace adsb;

// Vectors from "The 1090MHz Riddle" (Junzi Sun).
static const char *kIdent = "8D4840D6202CC371C32CE0576098";
static const char *kPosEven = "8D40621D58C382D690C8AC2863A7";
static const char *kPosOdd = "8D40621D58C386435CC412692AD6";
static const char *kVelocity = "8D485020994409940838175B284F";

static Frame frame_of(const char *hex)
{
    Frame frame{};
    const bool ok = parse_frame(hex, frame);
    assert(ok);
    return frame;
}

static void test_parse_frame()
{
    Frame frame{};
    assert(parse_frame("*8d4840d6202cc371c32ce0576098;\r\n", frame));
    assert(frame[0] == 0x8D && frame[1] == 0x48 && frame[13] == 0x98);
    assert(parse_frame(kIdent, frame));
    assert(parse_frame("  *8D4840D6202CC371C32CE0576098;  ", frame));
    // Short (56-bit) frames, truncated lines and non-hex are rejected.
    assert(!parse_frame("*5d4840d6a1b2c3;", frame));
    assert(!parse_frame("*8d4840d6202cc371c32ce05760;", frame));
    assert(!parse_frame("*8d4840d6202cc371c32ce0576098ff;", frame));
    assert(!parse_frame("*8d4840d6202cc371c32ce05760zz;", frame));
    assert(!parse_frame("", frame));
    assert(!parse_frame("*;", frame));
}

static void test_crc()
{
    for (const char *hex : {kIdent, kPosEven, kPosOdd, kVelocity}) {
        const Frame frame = frame_of(hex);
        assert(crc_ok(frame));
        // The parity of a whole valid frame, parity field included, is zero.
        assert(crc24(frame.data(), kFrameBytes) == 0);
        assert(downlink_format(frame) == 17);
    }
    Frame bad = frame_of(kIdent);
    bad[6] ^= 0x10;
    assert(!crc_ok(bad));
}

static void test_identification()
{
    const Frame frame = frame_of(kIdent);
    assert(icao(frame) == 0x4840D6);
    assert(type_code(frame) == 4);
    assert(callsign(frame) == "KLM1023");
    assert(callsign(frame_of(kVelocity)).empty());
    assert(format_icao(icao(frame)) == "4840D6");
}

static void test_position()
{
    const AirbornePosition even = airborne_position(frame_of(kPosEven));
    const AirbornePosition odd = airborne_position(frame_of(kPosOdd));
    assert(even.valid && !even.odd && odd.valid && odd.odd);
    assert(even.has_altitude && even.altitude_ft == 38000);
    assert(odd.has_altitude && odd.altitude_ft == 38000);
    assert(even.cpr_lat == 93000 && even.cpr_lon == 51372);
    assert(odd.cpr_lat == 74158 && odd.cpr_lon == 50194);

    // The even frame is the most recent one in the published example.
    const LatLon fix = cpr_global(even.cpr_lat, even.cpr_lon, odd.cpr_lat, odd.cpr_lon, false);
    assert(fix.valid);
    assert(std::fabs(fix.lat - 52.2572) < 0.0001);
    assert(std::fabs(fix.lon - 3.91937) < 0.0001);
    // Taking the odd frame as the latest gives a position a few hundred
    // metres away, not a different part of the world.
    const LatLon fix_odd = cpr_global(even.cpr_lat, even.cpr_lon, odd.cpr_lat, odd.cpr_lon, true);
    assert(fix_odd.valid);
    assert(std::fabs(fix_odd.lat - 52.2572) < 0.02);
    assert(std::fabs(fix_odd.lon - 3.91937) < 0.02);

    assert(!airborne_position(frame_of(kIdent)).valid);
}

static void test_velocity()
{
    const Velocity v = velocity(frame_of(kVelocity));
    assert(v.valid);
    assert(std::fabs(v.speed_kt - 159.20) < 0.01);
    assert(std::fabs(v.track_deg - 182.88) < 0.01);
    assert(!velocity(frame_of(kIdent)).valid);
}

static void test_table()
{
    Table table;
    assert(!table.feed("garbage", 0.0));
    assert(!table.feed("*5d4840d6a1b2c3;", 0.0));
    // A corrupted frame fails the CRC and creates nothing.
    assert(!table.feed("*8D4840D6202CC371C32CE0576099;", 0.0));
    assert(table.aircraft().empty());

    assert(table.feed("*8d4840d6202cc371c32ce0576098;", 1.0));
    assert(table.aircraft().size() == 1);
    const Aircraft *klm = table.find(0x4840D6);
    assert(klm && klm->callsign == "KLM1023" && !klm->has_position && !klm->has_altitude);
    assert(label(*klm) == "KLM1023");
    assert(format_flight_level(*klm) == "---");

    // Odd first, even two seconds later: position from the pair.
    assert(table.feed(std::string("*") + kPosOdd + ";", 2.0));
    const Aircraft *other = table.find(0x40621D);
    assert(other && !other->has_position && other->has_altitude && other->altitude_ft == 38000);
    assert(label(*other) == "40621D");
    assert(format_flight_level(*other) == "380");
    assert(table.feed(std::string("*") + kPosEven + ";", 4.0));
    other = table.find(0x40621D);
    assert(other && other->has_position);
    assert(std::fabs(other->lat - 52.2572) < 0.0001 && std::fabs(other->lon - 3.91937) < 0.0001);

    assert(table.feed(std::string("*") + kVelocity + ";", 5.0));
    const Aircraft *third = table.find(0x485020);
    assert(third && third->has_velocity && std::fabs(third->speed_kt - 159.20) < 0.01);
    assert(table.aircraft().size() == 3);

    // Ageing: KLM was last heard at 1 s, the others later.
    table.age(61.0);
    assert(table.aircraft().size() == 3);
    table.age(61.5);
    assert(table.aircraft().size() == 2 && !table.find(0x4840D6));
    table.age(200.0);
    assert(table.aircraft().empty());

    // A pair further apart than kCprPairSeconds gives no position.
    assert(table.feed(kPosOdd, 300.0));
    assert(table.feed(kPosEven, 311.0));
    other = table.find(0x40621D);
    assert(other && !other->has_position);
    // A fresh odd frame within the window of that even frame does.
    assert(table.feed(kPosOdd, 312.0));
    other = table.find(0x40621D);
    assert(other && other->has_position && std::fabs(other->lat - 52.2572) < 0.02);
    table.clear();
    assert(table.aircraft().empty());
}

int main()
{
    test_parse_frame();
    test_crc();
    test_identification();
    test_position();
    test_velocity();
    test_table();
    std::puts("adsb_model: ok");
    return 0;
}

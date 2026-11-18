// SPDX-License-Identifier: MIT

#include "../main/ui/model/location_model.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace location;

static bool near(double a, double b, double tolerance)
{
    return std::fabs(a - b) <= tolerance;
}

static Position at(double lat, double lon)
{
    Position p;
    p.valid = true;
    p.lat = lat;
    p.lon = lon;
    return p;
}

static void test_parse()
{
    Position p = parse("52.37,4.90\n");
    assert(p.valid && near(p.lat, 52.37, 1e-9) && near(p.lon, 4.90, 1e-9));
    p = parse("  -33.8688 , 151.2093  ");
    assert(p.valid && near(p.lat, -33.8688, 1e-9) && near(p.lon, 151.2093, 1e-9));
    p = parse("40.6413,-73.7781\nsecond line ignored");
    assert(p.valid && near(p.lon, -73.7781, 1e-9));
    assert(parse("90,180").valid);
    assert(parse("-90,-180").valid);

    assert(!parse("").valid);
    assert(!parse("52.37").valid);
    assert(!parse("52.37,").valid);
    assert(!parse(",4.9").valid);
    assert(!parse("90.01,0").valid);
    assert(!parse("0,180.5").valid);
    assert(!parse("-91,0").valid);
    assert(!parse("lat,lon").valid);
    assert(!parse("52.37,4.9E").valid);
    assert(!parse("52.37 4.90").valid);
    assert(!parse("nan,0").valid);
    assert(!parse("1e400,0").valid);

    assert(file_path("/home/pi") == "/home/pi/.config/cp0/location");
}

static void test_distance_and_bearing()
{
    const Position origin = at(0.0, 0.0);
    // One degree of arc on a 6371.0088 km sphere is 111.195 km.
    assert(near(distance_km(origin, at(0.0, 1.0)), 111.195, 0.01));
    assert(near(distance_km(origin, at(1.0, 0.0)), 111.195, 0.01));
    // One minute of latitude is one nautical mile to within the sphere model.
    assert(near(distance_nm(origin, at(1.0 / 60.0, 0.0)), 1.0, 0.002));
    assert(near(distance_km(origin, origin), 0.0, 1e-9));
    // Half way round the sphere.
    assert(near(distance_km(origin, at(0.0, 180.0)), 3.14159265358979 * 6371.0088, 0.01));
    assert(near(distance_km(at(90.0, 0.0), at(-90.0, 0.0)), 3.14159265358979 * 6371.0088, 0.01));

    assert(near(bearing_deg(origin, at(10.0, 0.0)), 0.0, 1e-6));
    assert(near(bearing_deg(origin, at(0.0, 10.0)), 90.0, 1e-6));
    assert(near(bearing_deg(origin, at(-10.0, 0.0)), 180.0, 1e-6));
    assert(near(bearing_deg(origin, at(0.0, -10.0)), 270.0, 1e-6));

    // Heathrow to JFK: about 5540 km, leaving on about 288 degrees.
    const Position lhr = at(51.4700, -0.4543);
    const Position jfk = at(40.6413, -73.7781);
    const double km = distance_km(lhr, jfk);
    assert(km > 5500.0 && km < 5580.0);
    assert(near(distance_nm(lhr, jfk), km / 1.852, 1e-9));
    const double out = bearing_deg(lhr, jfk);
    assert(out > 285.0 && out < 291.0);
    const double back = bearing_deg(jfk, lhr);
    assert(back > 48.0 && back < 55.0);
    for (double lon = -180.0; lon <= 180.0; lon += 7.5) {
        const double b = bearing_deg(lhr, at(-20.0, lon));
        assert(b >= 0.0 && b < 360.0);
    }
}

static void test_antenna()
{
    assert(near(quarter_wave_cm(1090e6), 6.876, 0.001));
    assert(near(quarter_wave_cm(137.1e6), 54.67, 0.01));
    assert(near(quarter_wave_cm(145.8e6), 51.40, 0.01));
    assert(near(dipole_leg_cm(137.1e6), 51.93, 0.01));
    assert(near(dipole_leg_cm(145.8e6), 48.83, 0.01));
    assert(quarter_wave_cm(0.0) == 0.0);
    assert(quarter_wave_cm(-5.0) == 0.0);
    char text[16];
    std::snprintf(text, sizeof(text), "%.1f", quarter_wave_cm(1090e6));
    assert(std::string(text) == "6.9");
    std::snprintf(text, sizeof(text), "%.1f", quarter_wave_cm(137.1e6));
    assert(std::string(text) == "54.7");
}

int main()
{
    test_parse();
    test_distance_and_bearing();
    test_antenna();
    std::puts("location_model: ok");
    return 0;
}

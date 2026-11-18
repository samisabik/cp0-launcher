/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <string>

// Observer position shared by the ADSB and Sat pages, with the great-circle
// maths they need and the antenna lengths their hint popups show. Free of
// LVGL and file access so it can be unit-tested.
namespace location {

struct Position {
    bool valid = false;
    double lat = 0.0; // degrees, north positive
    double lon = 0.0; // degrees, east positive
};

// "lat,lon" in decimal degrees, e.g. "52.37,4.90". Whitespace around the
// numbers is allowed; anything else, or a value out of range, is invalid.
Position parse(const std::string &text);

// The file the pages read the observer position from: <home>/.config/cp0/location
std::string file_path(const std::string &home);
// The shell line shown when that file is missing.
const char *set_hint();

// Great-circle distance on a sphere of mean Earth radius (haversine).
double distance_km(const Position &from, const Position &to);
double distance_nm(const Position &from, const Position &to);
// Initial bearing from `from` to `to`, degrees clockwise from north in [0, 360).
double bearing_deg(const Position &from, const Position &to);

// Free-space quarter wavelength, c / f / 4, with no velocity-factor
// correction, so a real element is cut slightly shorter.
double quarter_wave_cm(double hz);
// One leg of a V-dipole: the quarter wave times 0.95, a rule-of-thumb
// shortening factor for dipole legs, not a measured value.
double dipole_leg_cm(double hz);

} // namespace location

/*
 * SPDX-License-Identifier: MIT
 */

#include "sat_model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace sat {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kMu = 398600.4418;       // km^3/s^2
constexpr double kEarthRadiusKm = 6378.137;
constexpr double kJ2 = 1.08262668e-3;
constexpr double kFlattening = 1.0 / 298.257223563;
constexpr double kDaySeconds = 86400.0;
constexpr double kCoarseStepS = 30.0;

double rad(double deg)
{
    return deg * kPi / 180.0;
}

double wrap_two_pi(double angle)
{
    angle = std::fmod(angle, kTwoPi);
    return angle < 0.0 ? angle + kTwoPi : angle;
}

std::string trim(const std::string &text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

std::string upper(std::string text)
{
    for (char &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

// A fixed-column number; false when the field is blank or not numeric.
bool field(const std::string &line, std::size_t pos, std::size_t len, double &out)
{
    if (line.size() < pos + len) return false;
    const std::string token = trim(line.substr(pos, len));
    if (token.empty()) return false;
    char *stop = nullptr;
    out = std::strtod(token.c_str(), &stop);
    return stop && *stop == '\0' && std::isfinite(out);
}

// Days from 1970-01-01 to January 1st of `year`.
long days_to_year(int year)
{
    const long y = year - 1;
    return y * 365 + y / 4 - y / 100 + y / 400 - 719162;
}

bool is_element_line(const std::string &line, char number)
{
    return line.size() >= 69 && line[0] == number && line[1] == ' ';
}

// Mean motion in rad/s and semi-major axis with the J2 correction removed
// from the TLE (Kozai) mean motion.
struct Orbit {
    double n_tle = 0.0; // rad/s, as given
    double n0 = 0.0;    // rad/s, Brouwer
    double a = 0.0;     // km
};

Orbit orbit_of(const Tle &tle)
{
    Orbit orbit;
    orbit.n_tle = tle.mean_motion_rev_day * kTwoPi / kDaySeconds;
    if (!(orbit.n_tle > 0.0)) return orbit;
    const double cos_i = std::cos(rad(tle.inclination_deg));
    const double e2 = tle.eccentricity * tle.eccentricity;
    const double a1 = std::cbrt(kMu / (orbit.n_tle * orbit.n_tle));
    const double k = 0.75 * kJ2 * (3.0 * cos_i * cos_i - 1.0) / std::pow(1.0 - e2, 1.5);
    const double d1 = k * (kEarthRadiusKm / a1) * (kEarthRadiusKm / a1);
    const double a0 = a1 * (1.0 - d1 / 3.0 - d1 * d1 - 134.0 / 81.0 * d1 * d1 * d1);
    const double d0 = k * (kEarthRadiusKm / a0) * (kEarthRadiusKm / a0);
    orbit.n0 = orbit.n_tle / (1.0 + d0);
    orbit.a = a0 / (1.0 - d0);
    return orbit;
}

double elevation(const Tle &tle, const Observer &observer, double t)
{
    return look_angles(tle, observer, t).elevation_deg;
}

// Horizon crossing between t0 and t1, where the sign of the elevation differs.
double crossing(const Tle &tle, const Observer &observer, double t0, double t1)
{
    const bool rising = elevation(tle, observer, t1) > 0.0;
    for (int i = 0; i < 12; ++i) {
        const double mid = (t0 + t1) / 2.0;
        if ((elevation(tle, observer, mid) > 0.0) == rising)
            t1 = mid;
        else
            t0 = mid;
    }
    return (t0 + t1) / 2.0;
}

Pass finish_pass(const Tle &tle, const Observer &observer, double aos, double los)
{
    Pass pass;
    pass.aos = aos;
    pass.los = los;
    // Elevation has a single peak within a pass, so a ternary search finds it.
    double low = aos;
    double high = los;
    for (int i = 0; i < 24 && high - low > 1.0; ++i) {
        const double a = low + (high - low) / 3.0;
        const double b = high - (high - low) / 3.0;
        if (elevation(tle, observer, a) < elevation(tle, observer, b))
            low = a;
        else
            high = b;
    }
    pass.tmax = (low + high) / 2.0;
    pass.max_elevation_deg = elevation(tle, observer, pass.tmax);
    return pass;
}

} // namespace

int tle_checksum(const std::string &line)
{
    int sum = 0;
    for (std::size_t i = 0; i < line.size() && i < 68; ++i) {
        if (line[i] >= '0' && line[i] <= '9')
            sum += line[i] - '0';
        else if (line[i] == '-')
            sum += 1;
    }
    return sum % 10;
}

bool parse_tle(const std::string &name, const std::string &line1, const std::string &line2, Tle &out)
{
    if (!is_element_line(line1, '1') || !is_element_line(line2, '2')) return false;
    if (line1[68] - '0' != tle_checksum(line1) || line2[68] - '0' != tle_checksum(line2)) return false;
    if (line1.compare(2, 5, line2, 2, 5) != 0) return false;

    Tle tle;
    double satnum = 0.0;
    double year = 0.0;
    double day = 0.0;
    double ecc = 0.0;
    if (!field(line1, 2, 5, satnum) || !field(line1, 18, 2, year) || !field(line1, 20, 12, day)) return false;
    if (!field(line2, 8, 8, tle.inclination_deg) || !field(line2, 17, 8, tle.raan_deg) ||
        !field(line2, 26, 7, ecc) || !field(line2, 34, 8, tle.arg_perigee_deg) ||
        !field(line2, 43, 8, tle.mean_anomaly_deg) || !field(line2, 52, 11, tle.mean_motion_rev_day))
        return false;
    field(line1, 33, 10, tle.ndot2_rev_day2);
    tle.eccentricity = ecc * 1e-7;
    if (tle.mean_motion_rev_day <= 0.0 || tle.mean_motion_rev_day > 18.0 || tle.eccentricity >= 1.0 ||
        tle.inclination_deg < 0.0 || tle.inclination_deg > 180.0 || day < 1.0 || day >= 367.0)
        return false;
    tle.satnum = static_cast<int>(satnum);
    // Two-digit years: 57-99 are 1957-1999, 00-56 are 2000-2056.
    const int full_year = static_cast<int>(year) + (year < 57.0 ? 2000 : 1900);
    tle.epoch_unix = (static_cast<double>(days_to_year(full_year)) + day - 1.0) * kDaySeconds;
    tle.name = trim(name);
    if (tle.name.compare(0, 2, "0 ") == 0) tle.name = trim(tle.name.substr(2));
    if (tle.name.empty()) tle.name = std::to_string(tle.satnum);
    out = tle;
    return true;
}

std::vector<Tle> parse_tle_file(const std::string &text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) lines.push_back(line);
        start = end + 1;
    }
    std::vector<Tle> out;
    for (std::size_t i = 0; i + 1 < lines.size(); ++i) {
        if (!is_element_line(lines[i], '1') || !is_element_line(lines[i + 1], '2')) continue;
        const bool named = i > 0 && !is_element_line(lines[i - 1], '2') && !is_element_line(lines[i - 1], '1');
        Tle tle;
        if (parse_tle(named ? lines[i - 1] : std::string(), lines[i], lines[i + 1], tle) &&
            std::none_of(out.begin(), out.end(), [&](const Tle &t) { return t.satnum == tle.satnum; }))
            out.push_back(tle);
        ++i;
    }
    return out;
}

double period_minutes(const Tle &tle)
{
    return tle.mean_motion_rev_day > 0.0 ? 1440.0 / tle.mean_motion_rev_day : 0.0;
}

double semi_major_axis_km(const Tle &tle)
{
    return orbit_of(tle).a;
}

Vec3 position_eci_km(const Tle &tle, double unix_time)
{
    Vec3 out;
    const Orbit orbit = orbit_of(tle);
    if (!(orbit.a > 0.0)) return out;
    const double dt = unix_time - tle.epoch_unix;
    const double dt_days = dt / kDaySeconds;
    const double e = tle.eccentricity;
    const double inc = rad(tle.inclination_deg);
    const double cos_i = std::cos(inc);
    const double p = orbit.a * (1.0 - e * e);
    const double factor = 1.5 * kJ2 * (kEarthRadiusKm / p) * (kEarthRadiusKm / p) * orbit.n0;

    const double raan = rad(tle.raan_deg) - factor * cos_i * dt;
    const double argp = rad(tle.arg_perigee_deg) + factor * (2.0 - 2.5 * (1.0 - cos_i * cos_i)) * dt;
    // The TLE mean motion already contains the J2 secular rate of the mean
    // anomaly, so it is used as given.
    const double mean = wrap_two_pi(rad(tle.mean_anomaly_deg) + orbit.n_tle * dt +
                                    kTwoPi * tle.ndot2_rev_day2 * dt_days * dt_days);

    double ecc_anomaly = mean;
    for (int i = 0; i < 12; ++i) {
        const double step = (ecc_anomaly - e * std::sin(ecc_anomaly) - mean) / (1.0 - e * std::cos(ecc_anomaly));
        ecc_anomaly -= step;
        if (std::fabs(step) < 1e-12) break;
    }
    const double radius = orbit.a * (1.0 - e * std::cos(ecc_anomaly));
    const double true_anomaly =
        std::atan2(std::sqrt(1.0 - e * e) * std::sin(ecc_anomaly), std::cos(ecc_anomaly) - e);
    const double u = argp + true_anomaly;

    const double cos_u = std::cos(u);
    const double sin_u = std::sin(u);
    const double cos_o = std::cos(raan);
    const double sin_o = std::sin(raan);
    out.x = radius * (cos_o * cos_u - sin_o * sin_u * cos_i);
    out.y = radius * (sin_o * cos_u + cos_o * sin_u * cos_i);
    out.z = radius * sin_u * std::sin(inc);
    return out;
}

double gmst_rad(double unix_time)
{
    // IAU 1982 GMST, with UTC standing in for UT1.
    const double julian = unix_time / kDaySeconds + 2440587.5;
    const double t = (julian - 2451545.0) / 36525.0;
    double seconds = 67310.54841 + (876600.0 * 3600.0 + 8640184.812866) * t + 0.093104 * t * t - 6.2e-6 * t * t * t;
    seconds = std::fmod(seconds, kDaySeconds);
    if (seconds < 0.0) seconds += kDaySeconds;
    return wrap_two_pi(seconds / kDaySeconds * kTwoPi);
}

Look look_angles(const Tle &tle, const Observer &observer, double unix_time)
{
    Look look;
    const Vec3 eci = position_eci_km(tle, unix_time);
    const double theta = gmst_rad(unix_time);
    const double sx = eci.x * std::cos(theta) + eci.y * std::sin(theta);
    const double sy = -eci.x * std::sin(theta) + eci.y * std::cos(theta);
    const double sz = eci.z;

    // Observer on the WGS84 ellipsoid.
    const double lat = rad(observer.lat_deg);
    const double lon = rad(observer.lon_deg);
    const double e2 = kFlattening * (2.0 - kFlattening);
    const double n = kEarthRadiusKm / std::sqrt(1.0 - e2 * std::sin(lat) * std::sin(lat));
    const double h = observer.alt_m / 1000.0;
    const double ox = (n + h) * std::cos(lat) * std::cos(lon);
    const double oy = (n + h) * std::cos(lat) * std::sin(lon);
    const double oz = (n * (1.0 - e2) + h) * std::sin(lat);

    const double rx = sx - ox;
    const double ry = sy - oy;
    const double rz = sz - oz;
    const double south = std::sin(lat) * std::cos(lon) * rx + std::sin(lat) * std::sin(lon) * ry - std::cos(lat) * rz;
    const double east = -std::sin(lon) * rx + std::cos(lon) * ry;
    const double up = std::cos(lat) * std::cos(lon) * rx + std::cos(lat) * std::sin(lon) * ry + std::sin(lat) * rz;
    look.range_km = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (!(look.range_km > 0.0)) return look;
    look.elevation_deg = std::asin(std::clamp(up / look.range_km, -1.0, 1.0)) * 180.0 / kPi;
    double azimuth = std::atan2(east, -south) * 180.0 / kPi;
    if (azimuth < 0.0) azimuth += 360.0;
    if (azimuth >= 360.0) azimuth -= 360.0;
    look.azimuth_deg = azimuth;
    return look;
}

std::vector<Pass> find_passes(const Tle &tle, const Observer &observer, double start, double hours,
                              double min_elevation_deg)
{
    std::vector<Pass> passes;
    if (!(tle.mean_motion_rev_day > 0.0) || hours <= 0.0) return passes;
    const double end = start + hours * 3600.0;
    // A pass that begins inside the window is followed to its end, up to one
    // orbit beyond the window.
    const double hard_end = end + period_minutes(tle) * 60.0;
    double previous_time = start;
    bool up = elevation(tle, observer, start) > 0.0;
    double aos = start;
    for (double t = start + kCoarseStepS; t <= hard_end; t += kCoarseStepS) {
        if (!up && t > end) break;
        const bool now_up = elevation(tle, observer, t) > 0.0;
        if (now_up && !up) {
            aos = crossing(tle, observer, previous_time, t);
        } else if (!now_up && up) {
            const Pass pass = finish_pass(tle, observer, aos, crossing(tle, observer, previous_time, t));
            if (pass.max_elevation_deg >= min_elevation_deg) passes.push_back(pass);
        }
        up = now_up;
        previous_time = t;
    }
    if (up) {
        // Still above the horizon when the search ran out, as a high orbit is.
        const Pass pass = finish_pass(tle, observer, aos, previous_time);
        if (pass.max_elevation_deg >= min_elevation_deg) passes.push_back(pass);
    }
    return passes;
}

uint32_t downlink_hz(const std::string &name)
{
    // Unverified defaults: ISS 145.800 MHz (voice and SSTV downlink) and
    // 137.900 MHz for the METEOR-M2 3 and METEOR-M2 4 LRPT downlinks. They
    // were not checked against a current frequency list.
    const std::string n = upper(trim(name));
    if (n == "ISS" || n.compare(0, 11, "ISS (ZARYA)") == 0) return 145800000u;
    if (n.find("METEOR-M2 3") != std::string::npos) return 137900000u;
    if (n.find("METEOR-M2 4") != std::string::npos) return 137900000u;
    return 0;
}

std::vector<std::string> parse_watch(const std::string &text)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = trim(text.substr(start, end - start));
        if (!line.empty() && line[0] != '#') out.push_back(line);
        start = end + 1;
    }
    return out;
}

bool interesting(const std::string &name, const std::vector<std::string> &watch)
{
    const std::string n = upper(trim(name));
    // "ISS" alone would also match modules and debris catalogued as "ISS ...".
    if (n == "ISS" || n.compare(0, 11, "ISS (ZARYA)") == 0) return true;
    if (n.find("METEOR") != std::string::npos) return true;
    for (const std::string &pattern : watch) {
        const std::string p = upper(trim(pattern));
        if (!p.empty() && n.find(p) != std::string::npos) return true;
    }
    return false;
}

std::string short_name(const std::string &name, std::size_t max_chars)
{
    std::string text = trim(name);
    const std::size_t paren = text.find('(');
    if (paren != std::string::npos && paren > 0) text = trim(text.substr(0, paren));
    for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (text.size() > max_chars) {
        text.erase(std::remove_if(text.begin(), text.end(), [](char c) { return c == ' ' || c == '-'; }), text.end());
    }
    if (text.size() > max_chars && max_chars >= 4) {
        // Keep the tail: it usually holds the number that tells siblings apart.
        text = text.substr(0, max_chars - 3) + text.substr(text.size() - 3);
    } else if (text.size() > max_chars) {
        text.resize(max_chars);
    }
    return text;
}

std::string format_countdown(double seconds)
{
    long s = seconds > 0.0 ? static_cast<long>(seconds) : 0;
    char text[24];
    if (s < 3600)
        std::snprintf(text, sizeof(text), "%02ld:%02ld", s / 60, s % 60);
    else
        std::snprintf(text, sizeof(text), "%ld:%02ld:%02ld", std::min(s / 3600, 99L), (s / 60) % 60, s % 60);
    return text;
}

std::string format_duration(double seconds)
{
    const long minutes = std::clamp(std::lround(seconds / 60.0), 1L, 999L);
    return std::to_string(minutes) + "m";
}

std::string format_age(double seconds)
{
    const long s = seconds > 0.0 ? static_cast<long>(seconds) : 0;
    char text[16];
    if (s < 60)
        std::snprintf(text, sizeof(text), "%lds", s);
    else if (s < 3600)
        std::snprintf(text, sizeof(text), "%ldm", s / 60);
    else if (s < 86400)
        std::snprintf(text, sizeof(text), "%ldh", s / 3600);
    else
        std::snprintf(text, sizeof(text), "%ldd", std::min(s / 86400, 999L));
    return text;
}

std::string format_mhz(uint32_t hz)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%.3f", hz / 1e6);
    return text;
}

std::vector<uint8_t> wav_header(uint32_t sample_rate, uint32_t data_bytes)
{
    std::vector<uint8_t> h;
    auto text = [&h](const char *s) { h.insert(h.end(), s, s + 4); };
    auto u32 = [&h](uint32_t v) {
        for (int i = 0; i < 4; ++i) h.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    auto u16 = [&h](uint16_t v) {
        h.push_back(static_cast<uint8_t>(v & 0xFF));
        h.push_back(static_cast<uint8_t>(v >> 8));
    };
    text("RIFF");
    u32(36 + data_bytes);
    text("WAVE");
    text("fmt ");
    u32(16);
    u16(1); // PCM
    u16(1); // mono
    u32(sample_rate);
    u32(sample_rate * 2);
    u16(2);
    u16(16);
    text("data");
    u32(data_bytes);
    return h;
}

std::string record_file_name(const std::string &name, const std::string &stamp)
{
    std::string base;
    for (char c : short_name(name, 16))
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) base += c;
    if (base.empty()) base = "sat";
    return base + "-" + stamp + ".wav";
}

std::string record_command(uint32_t hz, const std::string &path)
{
    // rtl_fm options as understood from its usage text, unverified here:
    // -M fm narrow FM, -s output sample rate, "-" raw s16 samples on stdout.
    return "rtl_fm -f " + std::to_string(hz) + " -M fm -s " + std::to_string(kRecordRate) + " - >> '" + path +
           "' 2>/dev/null";
}

std::string update_command(const std::string &path, const std::string &status_path)
{
    const std::string base = "https://celestrak.org/NORAD/elements/gp.php?GROUP=";
    const std::string tmp = path + ".part";
    return "if ( curl -fsSL --max-time 40 '" + base + "stations&FORMAT=tle' && curl -fsSL --max-time 40 '" + base +
           "weather&FORMAT=tle' ) > '" + tmp + "' && [ -s '" + tmp + "' ] && mv '" + tmp + "' '" + path +
           "'; then echo ok > '" + status_path + "'; else rm -f '" + tmp + "'; echo fail > '" + status_path +
           "'; fi";
}

} // namespace sat

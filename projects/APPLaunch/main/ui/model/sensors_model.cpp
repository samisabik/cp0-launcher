/*
 * SPDX-License-Identifier: MIT
 */

#include "sensors_model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace sensors {

namespace {

// Value of a top-level-looking "key" in a flat JSON object: the text of a
// string without its quotes, or the bare token of a number or literal.
// rtl_433 prints flat objects, so no nesting is handled.
bool json_value(const std::string &json, const std::string &name, std::string &out)
{
    const std::string needle = "\"" + name + "\"";
    std::size_t at = 0;
    while ((at = json.find(needle, at)) != std::string::npos) {
        std::size_t pos = at + needle.size();
        at = pos;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
        if (pos >= json.size() || json[pos] != ':') continue;
        ++pos;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
        if (pos >= json.size()) return false;
        out.clear();
        if (json[pos] == '"') {
            for (++pos; pos < json.size() && json[pos] != '"'; ++pos) {
                if (json[pos] == '\\' && pos + 1 < json.size()) ++pos;
                out += json[pos];
            }
            return pos < json.size();
        }
        while (pos < json.size() && json[pos] != ',' && json[pos] != '}' && json[pos] != ' ' && json[pos] != '\t' &&
               json[pos] != '\r' && json[pos] != '\n')
            out += json[pos++];
        return !out.empty();
    }
    return false;
}

bool json_number(const std::string &json, const std::string &name, double &out)
{
    std::string text;
    if (!json_value(json, name, text) || text.empty()) return false;
    char *stop = nullptr;
    const double value = std::strtod(text.c_str(), &stop);
    if (!stop || *stop != '\0' || !std::isfinite(value)) return false;
    out = value;
    return true;
}

std::string lower(std::string text)
{
    for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

} // namespace

Reading parse_line(const std::string &json)
{
    Reading reading;
    const std::size_t open = json.find('{');
    if (open == std::string::npos || json.find('}', open) == std::string::npos) return reading;
    if (!json_value(json, "model", reading.model) || reading.model.empty()) {
        reading.model.clear();
        return reading;
    }
    reading.valid = true;
    json_value(json, "id", reading.id);
    json_value(json, "channel", reading.channel);
    json_value(json, "time", reading.time);
    double number = 0.0;
    if (json_number(json, "temperature_C", number)) {
        reading.has_temperature = true;
        reading.temperature_c = number;
    } else if (json_number(json, "temperature_F", number)) {
        reading.has_temperature = true;
        reading.temperature_c = (number - 32.0) * 5.0 / 9.0;
    }
    if (json_number(json, "humidity", number) && number >= 0.0 && number <= 100.0) {
        reading.has_humidity = true;
        reading.humidity = static_cast<int>(std::lround(number));
    }
    if (json_number(json, "battery_ok", number)) reading.battery_ok = number >= 0.5 ? 1 : 0;
    return reading;
}

std::vector<float> Sensor::history() const
{
    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(ring_count));
    const int start = (ring_head - ring_count + kHistorySize) % kHistorySize;
    for (int i = 0; i < ring_count; ++i) out.push_back(ring[static_cast<std::size_t>((start + i) % kHistorySize)]);
    return out;
}

std::string key(const Reading &reading)
{
    return reading.model + "|" + reading.id + "|" + reading.channel;
}

bool Table::add(const Reading &reading, double now)
{
    if (!reading.valid || (!reading.has_temperature && !reading.has_humidity)) return false;
    const std::string id = key(reading);
    auto it = std::find_if(sensors_.begin(), sensors_.end(), [&](const Sensor &s) { return key(s.latest) == id; });
    if (it == sensors_.end()) {
        if (sensors_.size() >= kMaxSensors) {
            sensors_.erase(std::min_element(sensors_.begin(), sensors_.end(), [](const Sensor &a, const Sensor &b) {
                return a.last_seen < b.last_seen;
            }));
        }
        Sensor fresh;
        sensors_.push_back(fresh);
        it = sensors_.end() - 1;
    }
    Sensor &sensor = *it;
    // A repeat that lacks a field keeps the value the sensor reported before.
    Reading merged = reading;
    if (!merged.has_temperature && sensor.latest.has_temperature) {
        merged.has_temperature = true;
        merged.temperature_c = sensor.latest.temperature_c;
    }
    if (!merged.has_humidity && sensor.latest.has_humidity) {
        merged.has_humidity = true;
        merged.humidity = sensor.latest.humidity;
    }
    if (merged.battery_ok < 0) merged.battery_ok = sensor.latest.battery_ok;
    sensor.latest = merged;
    sensor.last_seen = now;
    ++sensor.count;
    if (reading.has_temperature) {
        const float value = static_cast<float>(reading.temperature_c);
        if (sensor.ring_count > 0 && now - sensor.last_sample < kHistoryIntervalS) {
            sensor.ring[static_cast<std::size_t>((sensor.ring_head - 1 + kHistorySize) % kHistorySize)] = value;
        } else {
            sensor.ring[static_cast<std::size_t>(sensor.ring_head)] = value;
            sensor.ring_head = (sensor.ring_head + 1) % kHistorySize;
            sensor.ring_count = std::min(sensor.ring_count + 1, kHistorySize);
            sensor.last_sample = now;
        }
    }
    return true;
}

bool Table::add_line(const std::string &json, double now)
{
    return add(parse_line(json), now);
}

void Table::clear()
{
    sensors_.clear();
}

std::string format_age(double seconds)
{
    long s = seconds > 0.0 ? static_cast<long>(seconds) : 0;
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

std::string format_temperature(const Reading &reading)
{
    if (!reading.has_temperature) return "--.-";
    const double t = reading.temperature_c;
    char text[16];
    if (t <= -9.95 || t >= 99.95)
        std::snprintf(text, sizeof(text), "%ld", std::clamp(std::lround(t), -999L, 9999L));
    else
        std::snprintf(text, sizeof(text), "%4.1f", t);
    return text;
}

std::string format_name(const Reading &reading, std::size_t max_chars)
{
    std::string name = lower(reading.model);
    if (!reading.id.empty()) name += "  id " + lower(reading.id);
    if (!reading.channel.empty()) name += " ch " + lower(reading.channel);
    if (name.size() <= max_chars) return name;
    // Too long: keep the id and channel, which tell two sensors of one model
    // apart, as "id/channel" and shorten the model name instead.
    std::string tail;
    if (!reading.id.empty()) tail = " " + lower(reading.id);
    if (!reading.channel.empty()) tail += "/" + lower(reading.channel);
    if (tail.size() >= max_chars) return name.substr(0, max_chars);
    return lower(reading.model).substr(0, max_chars - tail.size()) + tail;
}

std::string format_frequency(uint32_t hz)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%.2f", hz / 1e6);
    return text;
}

std::string format_log_line(const Sensor &sensor, const std::string &stamp)
{
    const Reading &r = sensor.latest;
    std::string line = stamp.empty() ? r.model : stamp + " " + r.model;
    if (!r.id.empty()) line += " id " + r.id;
    if (!r.channel.empty()) line += " ch " + r.channel;
    char text[24];
    if (r.has_temperature) {
        std::snprintf(text, sizeof(text), " %.1fC", r.temperature_c);
        line += text;
    }
    if (r.has_humidity) {
        std::snprintf(text, sizeof(text), " %d%%", r.humidity);
        line += text;
    }
    if (r.battery_ok == 0) line += " batt low";
    return line;
}

std::vector<int> sparkline(const std::vector<float> &history, int width, int height)
{
    std::vector<int> out(static_cast<std::size_t>(std::max(width, 0)), -1);
    if (width <= 0 || height <= 0 || history.empty()) return out;
    const std::size_t shown = std::min(history.size(), static_cast<std::size_t>(width));
    const std::size_t first = history.size() - shown;
    float low = history[first];
    float high = history[first];
    for (std::size_t i = first; i < history.size(); ++i) {
        low = std::min(low, history[i]);
        high = std::max(high, history[i]);
    }
    if (high - low < 1.0f) {
        const float mid = (high + low) / 2.0f;
        low = mid - 0.5f;
        high = mid + 0.5f;
    }
    for (std::size_t i = 0; i < shown; ++i) {
        const float fraction = (history[first + i] - low) / (high - low);
        const int row = (height - 1) - static_cast<int>(std::lround(fraction * static_cast<float>(height - 1)));
        out[static_cast<std::size_t>(width) - shown + i] = std::clamp(row, 0, height - 1);
    }
    return out;
}

std::string command(uint32_t hz, const std::string &outpath)
{
    return "( rtl_433 -f " + std::to_string(hz) + " -F json 2>/dev/null; echo '" + kExitMark + "' ) >> '" + outpath +
           "'";
}

const char *source_name(Source source)
{
    return source == Source::Sdr ? "sdr" : "cc1101";
}

std::string decode_command(const std::string &inpath, const std::string &outpath)
{
    return "( rtl_433 -r '" + inpath + "' -F json 2>/dev/null; echo '" + kExitMark + "' ) >> '" + outpath + "'";
}

} // namespace sensors

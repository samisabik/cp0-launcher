/*
 * SPDX-License-Identifier: MIT
 */

#include "sdr_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace sdr {
namespace {

std::string json_string(const std::string &json, const std::string &key)
{
    const std::string needle = "\"" + key + "\"";
    std::size_t at = json.find(needle);
    if (at == std::string::npos) return {};
    at = json.find(':', at + needle.size());
    if (at == std::string::npos) return {};
    while (at + 1 < json.size() && (json[at + 1] == ' ' || json[at + 1] == ':')) ++at;
    if (at + 1 >= json.size()) return {};
    if (json[at + 1] == '"') {
        const std::size_t start = at + 2;
        const std::size_t end = json.find('"', start);
        return end == std::string::npos ? std::string() : json.substr(start, end - start);
    }
    const std::size_t start = at + 1;
    std::size_t end = start;
    while (end < json.size() && (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '.' ||
                                 json[end] == '-' || json[end] == '+'))
        ++end;
    return json.substr(start, end - start);
}

} // namespace

const char *demod_label(Demod demod)
{
    switch (demod) {
    case Demod::WFM:
        return "wfm";
    case Demod::NFM:
        return "nfm";
    case Demod::AM:
        return "am";
    }
    return "wfm";
}

uint32_t clamp_hz(int64_t hz)
{
    if (hz < kMinHz) return kMinHz;
    if (hz > kMaxHz) return kMaxHz;
    return static_cast<uint32_t>(hz);
}

std::string format_mhz(uint32_t hz)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%u.%03u", hz / 1000000u, (hz / 1000u) % 1000u);
    return text;
}

std::string format_hz(uint32_t hz)
{
    if (hz < 1000000u) {
        char text[24];
        std::snprintf(text, sizeof(text), "%u kHz", hz / 1000u);
        return text;
    }
    return format_mhz(hz) + " MHz";
}

std::string gain_label(int gain_db)
{
    if (gain_db < 0) return "auto";
    return std::to_string(gain_db) + " dB";
}

std::string detect_cmd()
{
    return "rtl_test -t 2>&1 | head -30";
}

std::string power_cmd(uint32_t center_hz, uint32_t span_hz, int gain_db, const std::string &outpath)
{
    span_hz = std::clamp<uint32_t>(span_hz, 200000u, kMaxSpanHz);
    const uint32_t low = clamp_hz(static_cast<int64_t>(center_hz) - span_hz / 2);
    const uint32_t high = clamp_hz(static_cast<int64_t>(center_hz) + span_hz / 2);
    const uint32_t bin = std::max<uint32_t>(1000u, span_hz / 320u);
    std::string cmd = "rtl_power -f " + std::to_string(low) + ":" + std::to_string(high) + ":" +
                      std::to_string(bin) + " -i 1";
    if (gain_db >= 0) cmd += " -g " + std::to_string(gain_db);
    cmd += " '" + outpath + "'";
    return cmd;
}

std::string listen_cmd(uint32_t hz, Demod demod, int gain_db)
{
    const char *mode = demod == Demod::WFM ? "wbfm" : demod == Demod::AM ? "am" : "fm";
    const int rate = demod == Demod::WFM ? 48000 : 24000;
    const int sample = demod == Demod::WFM ? 200000 : 24000;
    std::string fm = std::string("rtl_fm -M ") + mode + " -f " + std::to_string(hz) + " -s " +
                     std::to_string(sample) + " -r " + std::to_string(rate);
    if (gain_db >= 0) fm += " -g " + std::to_string(gain_db);
    fm += " -";
    return fm + " | aplay -q -r " + std::to_string(rate) + " -f S16_LE -t raw -c 1";
}

std::string decode_cmd(uint32_t hz, int gain_db, const std::string &outpath)
{
    std::string cmd = "rtl_433 -f " + std::to_string(hz) + " -F json";
    if (gain_db >= 0) cmd += " -g " + std::to_string(gain_db);
    cmd += " > '" + outpath + "' 2>/dev/null";
    return cmd;
}

Detect parse_detect(const std::string &output)
{
    Detect detect;
    if (output.find("No supported devices") != std::string::npos) return detect;
    const std::size_t at = output.find("Found ");
    if (at == std::string::npos) return detect;
    detect.present = true;
    // "Found 1 device(s):\n  0:  Realtek, RTL2838UHIDIR, SN: 00000001"
    const std::size_t line = output.find("0:", at);
    if (line != std::string::npos) {
        const std::size_t end = output.find('\n', line);
        std::string row = output.substr(line + 2, (end == std::string::npos ? output.size() : end) - line - 2);
        const std::size_t first = row.find_first_not_of(" \t");
        if (first != std::string::npos) row = row.substr(first);
        detect.name = row;
    }
    return detect;
}

PowerRow parse_power_row(const std::string &line)
{
    PowerRow row;
    std::stringstream stream(line);
    std::string field;
    std::vector<std::string> fields;
    while (std::getline(stream, field, ',')) {
        const std::size_t first = field.find_first_not_of(" \t");
        fields.push_back(first == std::string::npos ? "" : field.substr(first));
    }
    // date, time, low, high, step, samples, then dB values.
    if (fields.size() < 7) return row;
    row.low_hz = static_cast<uint32_t>(std::strtoul(fields[2].c_str(), nullptr, 10));
    row.step_hz = static_cast<uint32_t>(std::strtod(fields[4].c_str(), nullptr));
    if (row.step_hz == 0) return row;
    for (std::size_t i = 6; i < fields.size(); ++i) {
        if (fields[i].empty()) continue;
        row.db.push_back(static_cast<int>(std::lround(std::strtod(fields[i].c_str(), nullptr))));
    }
    row.valid = !row.db.empty();
    return row;
}

std::vector<int> power_columns(const PowerRow &row, int width)
{
    std::vector<int> columns(width, -120);
    if (!row.valid || width <= 0) return columns;
    const int bins = static_cast<int>(row.db.size());
    for (int x = 0; x < width; ++x) {
        const int bin = bins == 1 ? 0 : x * (bins - 1) / (width - 1 > 0 ? width - 1 : 1);
        columns[x] = row.db[std::clamp(bin, 0, bins - 1)];
    }
    return columns;
}

int level_index(int db, int floor_db, int ceil_db, int levels)
{
    if (ceil_db <= floor_db || levels <= 1) return 0;
    const int clamped = std::clamp(db, floor_db, ceil_db);
    return (clamped - floor_db) * (levels - 1) / (ceil_db - floor_db);
}

std::string summarize_433(const std::string &json)
{
    const std::string model = json_string(json, "model");
    if (model.empty()) return {};
    std::string line = model;
    const std::string id = json_string(json, "id");
    if (!id.empty()) line += " id " + id;
    const std::string temp = json_string(json, "temperature_C");
    if (!temp.empty()) line += " " + temp + "C";
    const std::string hum = json_string(json, "humidity");
    if (!hum.empty()) line += " " + hum + "%";
    const std::string button = json_string(json, "button");
    if (!button.empty()) line += " btn " + button;
    return line;
}

} // namespace sdr

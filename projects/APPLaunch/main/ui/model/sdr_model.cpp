/*
 * SPDX-License-Identifier: MIT
 */

#include "sdr_model.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
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

std::string iq_cmd(uint32_t center_hz, int gain_db)
{
    std::string cmd = "rtl_sdr -f " + std::to_string(center_hz + kTuneOffsetHz) + " -s " + std::to_string(kIqRate);
    if (gain_db >= 0) cmd += " -g " + std::to_string(gain_db);
    return cmd + " -";
}

std::vector<float> iq_spectrum_db(const uint8_t *iq, std::size_t bytes)
{
    constexpr float kPi = 3.14159265358979f;
    std::vector<float> out(kIqFft, -120.0f);
    if (!iq || bytes < static_cast<std::size_t>(2 * kIqFft)) return out;
    std::vector<std::complex<float>> a(kIqFft);
    float window_sum = 0.0f;
    for (int i = 0; i < kIqFft; ++i) {
        const float w = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) / static_cast<float>(kIqFft - 1));
        window_sum += w;
        a[i] = {w * (static_cast<float>(iq[2 * i]) - 127.5f) / 127.5f,
                w * (static_cast<float>(iq[2 * i + 1]) - 127.5f) / 127.5f};
    }
    for (std::size_t i = 1, j = 0; i < a.size(); ++i) {
        std::size_t bit = a.size() >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= a.size(); len <<= 1) {
        const float angle = -2.0f * kPi / static_cast<float>(len);
        const std::complex<float> step(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < a.size(); i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k];
                const std::complex<float> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
    for (int i = 0; i < kIqFft; ++i) {
        const int src = (i + kIqFft / 2) % kIqFft;
        const float mag = std::abs(a[src]) / window_sum;
        out[i] = std::max(-120.0f, 20.0f * std::log10(mag + 1e-9f));
    }
    const int dc = kIqFft / 2;
    const float fill = 0.5f * (out[dc - 3] + out[dc + 3]);
    for (int i = dc - 2; i <= dc + 2; ++i) out[i] = fill;
    return out;
}

std::vector<float> span_columns(const std::vector<float> &bins, uint32_t span_hz, int width, int offset_hz)
{
    std::vector<float> out(std::max(0, width), -120.0f);
    if (bins.empty() || width <= 0) return out;
    const double used = std::clamp(static_cast<double>(span_hz) / kIqRate, 0.01, 1.0) * static_cast<double>(bins.size());
    const double shift = static_cast<double>(offset_hz) / kIqRate * static_cast<double>(bins.size());
    const double first = (static_cast<double>(bins.size()) - used) / 2.0 - shift;
    for (int x = 0; x < width; ++x) {
        int lo = static_cast<int>(first + used * x / width);
        int hi = static_cast<int>(first + used * (x + 1) / width);
        lo = std::clamp(lo, 0, static_cast<int>(bins.size()) - 1);
        hi = std::clamp(std::max(hi, lo + 1), lo + 1, static_cast<int>(bins.size()));
        float best = -120.0f;
        for (int i = lo; i < hi; ++i) best = std::max(best, bins[i]);
        out[x] = best;
    }
    return out;
}

void Demodulator::process(const uint8_t *iq, std::size_t bytes, std::vector<int16_t> &out)
{
    constexpr double kTwoPi = 6.283185307179586;
    constexpr int kDecim1 = 10;
    constexpr int kDecim2 = 5;
    const double step = kTwoPi * kTuneOffsetHz / kIqRate;
    const double step_re = std::cos(step), step_im = std::sin(step);
    const float deemph_alpha = 1.0f - std::exp(-1.0f / (kAudioRate * 50e-6f));
    auto push = [&out](float v) {
        out.push_back(static_cast<int16_t>(std::clamp(v, -32767.0f, 32767.0f)));
    };
    for (std::size_t n = 0; n + 1 < bytes; n += 2) {
        const float i0 = (static_cast<float>(iq[n]) - 127.5f) / 127.5f;
        const float q0 = (static_cast<float>(iq[n + 1]) - 127.5f) / 127.5f;
        acc1_re_ += static_cast<float>(i0 * phase_re_ - q0 * phase_im_);
        acc1_im_ += static_cast<float>(i0 * phase_im_ + q0 * phase_re_);
        const double re = phase_re_ * step_re - phase_im_ * step_im;
        phase_im_ = phase_re_ * step_im + phase_im_ * step_re;
        phase_re_ = re;
        if (++count1_ < kDecim1) continue;
        count1_ = 0;
        const float x_re = acc1_re_ / kDecim1, x_im = acc1_im_ / kDecim1;
        acc1_re_ = acc1_im_ = 0.0f;

        if (demod_ == Demod::WFM) {
            const float d = std::atan2(x_im * prev_re_ - x_re * prev_im_, x_re * prev_re_ + x_im * prev_im_);
            prev_re_ = x_re;
            prev_im_ = x_im;
            acc2_ += d;
            if (++count2_ < kDecim2) continue;
            count2_ = 0;
            const float y = acc2_ / kDecim2;
            acc2_ = 0.0f;
            deemph_ += deemph_alpha * (y - deemph_);
            push(deemph_ * 12000.0f);
            continue;
        }
        acc2_re_ += x_re;
        acc2_im_ += x_im;
        if (++count2_ < kDecim2) continue;
        count2_ = 0;
        const float z_re = acc2_re_ / kDecim2, z_im = acc2_im_ / kDecim2;
        acc2_re_ = acc2_im_ = 0.0f;
        if (demod_ == Demod::NFM) {
            const float d = std::atan2(z_im * prev_re_ - z_re * prev_im_, z_re * prev_re_ + z_im * prev_im_);
            prev_re_ = z_re;
            prev_im_ = z_im;
            deemph_ += 0.4f * (d - deemph_);
            push(deemph_ * 30000.0f);
        } else {
            const float m = std::sqrt(z_re * z_re + z_im * z_im);
            dc_ += 0.001f * (m - dc_);
            push((m - dc_) / std::max(dc_, 1e-3f) * 12000.0f);
        }
    }
    const double norm = std::sqrt(phase_re_ * phase_re_ + phase_im_ * phase_im_);
    phase_re_ /= norm;
    phase_im_ /= norm;
}

float median_db(std::vector<float> values)
{
    if (values.empty()) return -120.0f;
    const std::size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<long>(mid), values.end());
    return values[mid];
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

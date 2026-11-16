/*
 * SPDX-License-Identifier: MIT
 */

#include "audio_model.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>

namespace audio {

namespace {

constexpr float kPi = 3.14159265358979f;

void fft(std::vector<std::complex<float>> &a)
{
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const float angle = -2.0f * kPi / static_cast<float>(len);
        const std::complex<float> step(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len) {
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
}

const std::vector<float> &hann()
{
    static const std::vector<float> window = [] {
        std::vector<float> w(kFftSize);
        for (int i = 0; i < kFftSize; ++i)
            w[i] = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) / static_cast<float>(kFftSize - 1));
        return w;
    }();
    return window;
}

} // namespace

std::vector<float> spectrum_db(const int16_t *samples, std::size_t count)
{
    std::vector<float> out(kFftSize / 2, kFloorDb);
    if (!samples || count < static_cast<std::size_t>(kFftSize)) return out;
    const std::vector<float> &window = hann();
    std::vector<std::complex<float>> buffer(kFftSize);
    float window_sum = 0.0f;
    for (int i = 0; i < kFftSize; ++i) {
        buffer[i] = std::complex<float>(window[i] * static_cast<float>(samples[i]) / 32768.0f, 0.0f);
        window_sum += window[i];
    }
    fft(buffer);
    const float scale = 2.0f / window_sum;
    for (int i = 0; i < kFftSize / 2; ++i) {
        const float magnitude = std::abs(buffer[i]) * scale;
        out[i] = std::max(kFloorDb, 20.0f * std::log10(magnitude + 1e-9f));
    }
    return out;
}

Peak find_peak(const std::vector<float> &db, int sample_rate, float min_hz, float max_hz, float gate_db)
{
    Peak peak;
    if (db.size() < 3 || sample_rate <= 0) return peak;
    const float bin_hz = static_cast<float>(sample_rate) / static_cast<float>(db.size() * 2);
    const int first = std::max(1, static_cast<int>(std::ceil(min_hz / bin_hz)));
    const int last = std::min(static_cast<int>(db.size()) - 2, static_cast<int>(std::floor(max_hz / bin_hz)));
    int best = -1;
    for (int i = first; i <= last; ++i)
        if (best < 0 || db[i] > db[best]) best = i;
    if (best < 0) return peak;
    const float a = db[best - 1];
    const float b = db[best];
    const float c = db[best + 1];
    const float denom = a - 2.0f * b + c;
    const float offset = std::fabs(denom) > 1e-6f ? 0.5f * (a - c) / denom : 0.0f;
    peak.hz = (static_cast<float>(best) + std::clamp(offset, -0.5f, 0.5f)) * bin_hz;
    peak.db = b - 0.25f * (a - c) * offset;
    peak.valid = peak.db >= gate_db;
    return peak;
}

float rms_dbfs(const int16_t *samples, std::size_t count)
{
    if (!samples || count == 0) return kFloorDb;
    double sum = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double v = samples[i] / 32768.0;
        sum += v * v;
    }
    const double rms = std::sqrt(sum / static_cast<double>(count));
    return std::max(kFloorDb, static_cast<float>(20.0 * std::log10(rms + 1e-9)));
}

Note nearest_note(float hz)
{
    static const char *const kNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    Note note;
    if (!(hz > 0.0f)) return note;
    const float midi = 69.0f + 12.0f * std::log2(hz / 440.0f);
    const int nearest = static_cast<int>(std::lround(midi));
    note.cents = static_cast<int>(std::lround((midi - static_cast<float>(nearest)) * 100.0f));
    const int index = ((nearest % 12) + 12) % 12;
    const int octave = nearest / 12 - 1 - (nearest < 0 && nearest % 12 != 0 ? 1 : 0);
    note.name = std::string(kNames[index]) + std::to_string(octave);
    return note;
}

float log_position(float hz, float min_hz, float max_hz)
{
    if (!(hz > 0.0f) || min_hz <= 0.0f || max_hz <= min_hz) return 0.0f;
    return std::clamp(std::log(hz / min_hz) / std::log(max_hz / min_hz), 0.0f, 1.0f);
}

std::vector<float> log_bands(const std::vector<float> &db, int sample_rate, int bands, float min_hz, float max_hz)
{
    std::vector<float> out(std::max(0, bands), kFloorDb);
    if (db.empty() || bands <= 0 || sample_rate <= 0 || min_hz <= 0.0f || max_hz <= min_hz) return out;
    const float bin_hz = static_cast<float>(sample_rate) / static_cast<float>(db.size() * 2);
    const float ratio = max_hz / min_hz;
    for (int band = 0; band < bands; ++band) {
        const float lo = min_hz * std::pow(ratio, static_cast<float>(band) / static_cast<float>(bands));
        const float hi = min_hz * std::pow(ratio, static_cast<float>(band + 1) / static_cast<float>(bands));
        int first = static_cast<int>(std::floor(lo / bin_hz));
        int last = static_cast<int>(std::ceil(hi / bin_hz));
        first = std::clamp(first, 0, static_cast<int>(db.size()) - 1);
        last = std::clamp(last, first, static_cast<int>(db.size()) - 1);
        float best = kFloorDb;
        for (int i = first; i <= last; ++i) best = std::max(best, db[i]);
        out[band] = best;
    }
    return out;
}

std::string format_hz(float hz)
{
    char text[24];
    if (hz >= 10000.0f)
        std::snprintf(text, sizeof(text), "%.2f kHz", hz / 1000.0f);
    else
        std::snprintf(text, sizeof(text), "%.1f Hz", hz);
    return text;
}

} // namespace audio

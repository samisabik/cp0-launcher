/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Pure audio analysis for the Audio page: windowed FFT magnitudes, the
// dominant peak with sub-bin interpolation, note naming and log bands.
namespace audio {

constexpr int kSampleRate = 32000;
constexpr int kFftSize = 4096;
constexpr float kMinHz = 40.0f;
constexpr float kMaxHz = 15000.0f;
constexpr float kFloorDb = -100.0f;

// Magnitude per bin (kFftSize / 2 entries) in dBFS: a full-scale sine at a
// bin centre reads 0 dB. `samples` must hold at least kFftSize values.
std::vector<float> spectrum_db(const int16_t *samples, std::size_t count);

struct Peak {
    bool valid = false;
    float hz = 0.0f;
    float db = kFloorDb;
};

// Loudest bin between min_hz and max_hz, refined by parabolic interpolation.
// Invalid when it is below gate_db.
Peak find_peak(const std::vector<float> &db, int sample_rate, float min_hz, float max_hz, float gate_db);

// RMS level of the block in dBFS.
float rms_dbfs(const int16_t *samples, std::size_t count);

struct Note {
    std::string name; // e.g. "A4", "C#5"
    int cents = 0;    // -50..+50 from that note
};

// Nearest equal-tempered note (A4 = 440 Hz).
Note nearest_note(float hz);

// Highest bin value inside each of `bands` log-spaced bands from min_hz to max_hz.
std::vector<float> log_bands(const std::vector<float> &db, int sample_rate, int bands, float min_hz, float max_hz);

// Log-scale position (0..1) of hz between min_hz and max_hz.
float log_position(float hz, float min_hz, float max_hz);

std::string format_hz(float hz);

} // namespace audio

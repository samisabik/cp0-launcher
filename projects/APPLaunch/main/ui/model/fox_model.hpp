/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "spectrum_model.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Pure logic for the Fox page (transmitter hunting on one frequency), free of
// LVGL and hardware access so it can be unit-tested on the host.
namespace fox {

constexpr int kBarMinDbm = -100;
constexpr int kBarMaxDbm = -40;
// Share of each new reading in the smoothed level.
constexpr float kSmoothing = 0.3f;
constexpr float kTrendDeadBandDb = 2.0f;
// The beep is silent below the floor and fastest at the top of the bar.
constexpr int kBeepFloorDbm = -90;
constexpr int kBeepSlowMs = 800;
constexpr int kBeepFastMs = 70;

// 0..1 position of a level on the bar, kBarMinDbm..kBarMaxDbm.
float bar_fraction(float dbm);

// The channels the Spectrum finder watches, in the same order.
std::size_t frequency_count();
uint32_t frequency_hz(std::size_t index);
std::size_t default_frequency_index(); // 433.92 MHz
// Next or previous channel, wrapping at both ends.
std::size_t step_frequency(std::size_t index, int delta);
// "433.92", "869.525": MHz with a trailing zero dropped.
std::string format_mhz(uint32_t hz);

enum class Trend { Steady, Warmer, Colder };
const char *trend_label(Trend trend);

// Milliseconds between beeps for a level, 0 for silence.
int beep_interval_ms(float dbm);

// Smoothed level, peak hold, trend and zero reference for one frequency.
class Tracker
{
public:
    // trend_window is the number of readings the trend looks back over,
    // about one second's worth.
    explicit Tracker(int trend_window = 25);

    void reset();
    void add(int dbm);
    bool valid() const { return count_ > 0; }
    float level() const { return level_; }

    float peak() const { return peak_; }
    void reset_peak() { peak_ = level_; }

    // Newest quarter of the window against the oldest quarter; a change
    // inside the dead band, or a window that is not full yet, is steady.
    Trend trend() const;

    void set_zero();
    void clear_zero() { zeroed_ = false; }
    bool zeroed() const { return zeroed_; }
    float zero() const { return zero_; }
    float delta() const { return zeroed_ ? level_ - zero_ : 0.0f; }

private:
    std::vector<float> window_;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    float level_ = static_cast<float>(kBarMinDbm);
    float peak_ = static_cast<float>(kBarMinDbm);
    float zero_ = 0.0f;
    bool zeroed_ = false;
};

// Scrolling strip: one column per `samples_per_column` readings, holding the
// strongest of them, newest last.
class History
{
public:
    explicit History(std::size_t columns = 300, int samples_per_column = 2);

    void clear();
    void push(float dbm);
    const std::vector<float> &columns() const { return columns_; }
    std::size_t capacity() const { return capacity_; }
    // Time the full strip covers at a reading rate.
    int seconds(float readings_per_second) const;

private:
    std::size_t capacity_;
    int per_column_;
    int pending_ = 0;
    float pending_max_ = 0.0f;
    std::vector<float> columns_;
};

// Tone bursts as 16-bit mono samples for a continuous audio stream: silence
// with a short beep every interval.
class Beeper
{
public:
    static constexpr int kRate = 16000;
    static constexpr int kToneHz = 1000;
    static constexpr int kBeepSamples = kRate * 35 / 1000;
    static constexpr int kRampSamples = kRate * 2 / 1000;
    static constexpr int kAmplitude = 9000;

    // Fills `count` samples. interval_ms is the current beep interval, 0 for
    // silence; a beep already sounding always finishes.
    void render(int16_t *out, std::size_t count, int interval_ms);

private:
    int tone_left_ = 0;
    // Starts past any interval so the first beep sounds at once.
    int since_start_ = kRate * 10;
};

} // namespace fox

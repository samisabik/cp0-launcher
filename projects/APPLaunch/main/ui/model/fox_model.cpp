/*
 * SPDX-License-Identifier: MIT
 */

#include "fox_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fox {

float bar_fraction(float dbm)
{
    const float f = (dbm - kBarMinDbm) / static_cast<float>(kBarMaxDbm - kBarMinDbm);
    return std::clamp(f, 0.0f, 1.0f);
}

std::size_t frequency_count()
{
    return spectrum::kFinderChannels.size();
}

uint32_t frequency_hz(std::size_t index)
{
    return spectrum::kFinderChannels[index < frequency_count() ? index : 0].hz;
}

std::size_t default_frequency_index()
{
    for (std::size_t i = 0; i < frequency_count(); ++i)
        if (spectrum::kFinderChannels[i].hz == 433920000u) return i;
    return 0;
}

std::size_t step_frequency(std::size_t index, int delta)
{
    const long count = static_cast<long>(frequency_count());
    const long current = index < frequency_count() ? static_cast<long>(index) : 0;
    return static_cast<std::size_t>(((current + delta) % count + count) % count);
}

std::string format_mhz(uint32_t hz)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%u.%03u", static_cast<unsigned>(hz / 1000000u),
                  static_cast<unsigned>((hz % 1000000u + 500u) / 1000u % 1000u));
    std::string out = text;
    if (out.back() == '0') out.pop_back();
    return out;
}

const char *trend_label(Trend trend)
{
    return trend == Trend::Warmer ? "warmer" : trend == Trend::Colder ? "colder" : "steady";
}

int beep_interval_ms(float dbm)
{
    if (dbm < static_cast<float>(kBeepFloorDbm)) return 0;
    const float f = std::clamp((dbm - kBeepFloorDbm) / static_cast<float>(kBarMaxDbm - kBeepFloorDbm), 0.0f, 1.0f);
    return kBeepSlowMs + static_cast<int>(std::lround(static_cast<float>(kBeepFastMs - kBeepSlowMs) * f));
}

Tracker::Tracker(int trend_window) : window_(static_cast<std::size_t>(std::max(trend_window, 4)), 0.0f) {}

void Tracker::reset()
{
    head_ = 0;
    count_ = 0;
    level_ = static_cast<float>(kBarMinDbm);
    peak_ = static_cast<float>(kBarMinDbm);
    zeroed_ = false;
}

void Tracker::add(int dbm)
{
    const float value = static_cast<float>(dbm);
    level_ = count_ == 0 ? value : level_ + kSmoothing * (value - level_);
    peak_ = count_ == 0 ? level_ : std::max(peak_, level_);
    window_[head_] = level_;
    head_ = (head_ + 1) % window_.size();
    if (count_ < window_.size()) ++count_;
}

Trend Tracker::trend() const
{
    if (count_ < window_.size()) return Trend::Steady;
    // With a full ring, head_ is the oldest entry.
    const std::size_t size = window_.size();
    const std::size_t quarter = size / 4;
    float oldest = 0.0f;
    float newest = 0.0f;
    for (std::size_t i = 0; i < quarter; ++i) {
        oldest += window_[(head_ + i) % size];
        newest += window_[(head_ + size - 1 - i) % size];
    }
    const float change = (newest - oldest) / static_cast<float>(quarter);
    if (change > kTrendDeadBandDb) return Trend::Warmer;
    if (change < -kTrendDeadBandDb) return Trend::Colder;
    return Trend::Steady;
}

void Tracker::set_zero()
{
    zero_ = level_;
    zeroed_ = true;
}

History::History(std::size_t columns, int samples_per_column)
    : capacity_(std::max<std::size_t>(columns, 1)), per_column_(std::max(samples_per_column, 1))
{
    columns_.reserve(capacity_);
}

void History::clear()
{
    columns_.clear();
    pending_ = 0;
}

void History::push(float dbm)
{
    pending_max_ = pending_ == 0 ? dbm : std::max(pending_max_, dbm);
    if (++pending_ < per_column_) return;
    pending_ = 0;
    if (columns_.size() >= capacity_) columns_.erase(columns_.begin());
    columns_.push_back(pending_max_);
}

int History::seconds(float readings_per_second) const
{
    if (readings_per_second <= 0.0f) return 0;
    return static_cast<int>(std::lround(static_cast<float>(capacity_) * static_cast<float>(per_column_) /
                                        readings_per_second));
}

void Beeper::render(int16_t *out, std::size_t count, int interval_ms)
{
    const int interval = interval_ms > 0 ? static_cast<int>(static_cast<long>(interval_ms) * kRate / 1000) : 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (tone_left_ == 0 && interval > 0 && since_start_ >= interval) {
            tone_left_ = kBeepSamples;
            since_start_ = 0;
        }
        int16_t sample = 0;
        if (tone_left_ > 0) {
            const int index = kBeepSamples - tone_left_;
            const int edge = std::min(index, tone_left_ - 1);
            const float gain = edge < kRampSamples ? static_cast<float>(edge) / kRampSamples : 1.0f;
            const float phase = 2.0f * 3.14159265f * static_cast<float>(index * kToneHz % kRate) / kRate;
            sample = static_cast<int16_t>(std::lround(kAmplitude * gain * std::sin(phase)));
            --tone_left_;
        }
        out[i] = sample;
        if (since_start_ < kRate * 10) ++since_start_;
    }
}

} // namespace fox

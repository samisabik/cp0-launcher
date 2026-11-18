// SPDX-License-Identifier: MIT

#include "../main/ui/model/fox_model.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace fox;

namespace {

bool near(float a, float b, float tolerance = 0.001f)
{
    return std::fabs(a - b) <= tolerance;
}

void test_bar()
{
    assert(near(bar_fraction(-100.0f), 0.0f));
    assert(near(bar_fraction(-40.0f), 1.0f));
    assert(near(bar_fraction(-70.0f), 0.5f));
    assert(near(bar_fraction(-85.0f), 0.25f));
    assert(near(bar_fraction(-130.0f), 0.0f));
    assert(near(bar_fraction(-10.0f), 1.0f));
}

void test_frequencies()
{
    assert(frequency_count() == spectrum::kFinderChannels.size());
    for (std::size_t i = 0; i < frequency_count(); ++i) assert(frequency_hz(i) == spectrum::kFinderChannels[i].hz);
    assert(frequency_hz(999) == spectrum::kFinderChannels[0].hz);
    assert(frequency_hz(default_frequency_index()) == 433920000u);

    const std::size_t last = frequency_count() - 1;
    assert(step_frequency(0, 1) == 1);
    assert(step_frequency(3, -1) == 2);
    assert(step_frequency(last, 1) == 0);
    assert(step_frequency(0, -1) == last);
    assert(step_frequency(999, 1) == 1);
    // Stepping all the way round visits every channel once.
    std::size_t index = default_frequency_index();
    for (std::size_t i = 0; i < frequency_count(); ++i) index = step_frequency(index, 1);
    assert(index == default_frequency_index());

    assert(format_mhz(433920000u) == "433.92");
    assert(format_mhz(868350000u) == "868.35");
    assert(format_mhz(869525000u) == "869.525");
    assert(format_mhz(315000000u) == "315.00");
}

void test_smoothing_and_peak()
{
    Tracker tracker;
    assert(!tracker.valid());
    tracker.add(-80);
    assert(tracker.valid());
    assert(near(tracker.level(), -80.0f)); // the first reading seeds the level
    assert(near(tracker.peak(), -80.0f));
    tracker.add(-60);
    assert(near(tracker.level(), -80.0f + kSmoothing * 20.0f));
    tracker.add(-60);
    assert(near(tracker.level(), -74.0f + kSmoothing * 14.0f));
    for (int i = 0; i < 40; ++i) tracker.add(-60);
    assert(near(tracker.level(), -60.0f, 0.01f));
    assert(near(tracker.peak(), tracker.level()));

    // The peak holds while the level falls, and reset drops it to the level.
    const float held = tracker.peak();
    for (int i = 0; i < 40; ++i) tracker.add(-95);
    assert(near(tracker.level(), -95.0f, 0.01f));
    assert(near(tracker.peak(), held));
    tracker.reset_peak();
    assert(near(tracker.peak(), tracker.level()));
    tracker.add(-90);
    assert(tracker.peak() > -95.0f && near(tracker.peak(), tracker.level()));

    // A single spike moves the smoothed level by only its share.
    Tracker spike;
    for (int i = 0; i < 30; ++i) spike.add(-90);
    spike.add(-50);
    assert(near(spike.level(), -90.0f + kSmoothing * 40.0f, 0.01f));

    tracker.reset();
    assert(!tracker.valid());
    assert(near(tracker.peak(), static_cast<float>(kBarMinDbm)));
}

void test_trend()
{
    Tracker tracker(24);
    // Not enough history yet.
    for (int i = 0; i < 23; ++i) {
        tracker.add(-90 + i);
        assert(tracker.trend() == Trend::Steady);
    }
    tracker.add(-67);
    assert(tracker.trend() == Trend::Warmer);

    // Holding still settles to steady, with readings wobbling by a dB.
    for (int i = 0; i < 60; ++i) tracker.add(-66 + (i % 2));
    assert(tracker.trend() == Trend::Steady);

    for (int i = 0; i < 24; ++i) tracker.add(-66 - i);
    assert(tracker.trend() == Trend::Colder);

    // A slow drift of 1 dB over the window stays inside the dead band.
    Tracker drift(24);
    for (int i = 0; i < 48; ++i) drift.add(-70);
    for (int i = 0; i < 24; ++i) drift.add(i < 12 ? -70 : -69);
    assert(drift.trend() == Trend::Steady);

    // A step of 10 dB reads warmer, then steady once the window passes it.
    Tracker step(24);
    for (int i = 0; i < 24; ++i) step.add(-80);
    for (int i = 0; i < 8; ++i) step.add(-70);
    assert(step.trend() == Trend::Warmer);
    for (int i = 0; i < 48; ++i) step.add(-70);
    assert(step.trend() == Trend::Steady);

    step.reset();
    assert(step.trend() == Trend::Steady);

    assert(std::string(trend_label(Trend::Warmer)) == "warmer");
    assert(std::string(trend_label(Trend::Colder)) == "colder");
    assert(std::string(trend_label(Trend::Steady)) == "steady");
}

void test_zero()
{
    Tracker tracker;
    for (int i = 0; i < 40; ++i) tracker.add(-75);
    assert(!tracker.zeroed());
    assert(near(tracker.delta(), 0.0f));
    tracker.set_zero();
    assert(tracker.zeroed());
    assert(near(tracker.zero(), -75.0f, 0.01f));
    for (int i = 0; i < 40; ++i) tracker.add(-63);
    assert(near(tracker.delta(), 12.0f, 0.05f));
    for (int i = 0; i < 40; ++i) tracker.add(-81);
    assert(near(tracker.delta(), -6.0f, 0.05f));
    tracker.clear_zero();
    assert(!tracker.zeroed() && near(tracker.delta(), 0.0f));
    tracker.set_zero();
    tracker.reset();
    assert(!tracker.zeroed());
}

void test_beep_interval()
{
    assert(beep_interval_ms(-100.0f) == 0);
    assert(beep_interval_ms(-90.5f) == 0);
    assert(beep_interval_ms(static_cast<float>(kBeepFloorDbm)) == kBeepSlowMs);
    assert(beep_interval_ms(static_cast<float>(kBarMaxDbm)) == kBeepFastMs);
    assert(beep_interval_ms(-10.0f) == kBeepFastMs);
    assert(beep_interval_ms(-65.0f) == (kBeepSlowMs + kBeepFastMs) / 2);
    int previous = beep_interval_ms(-90.0f);
    for (int dbm = -89; dbm <= -40; ++dbm) {
        const int interval = beep_interval_ms(static_cast<float>(dbm));
        assert(interval > 0 && interval < previous);
        previous = interval;
    }
}

// Number of tone bursts: a sample that sounds after at least 5 ms of silence.
int count_beeps(const std::vector<int16_t> &samples)
{
    int beeps = 0;
    int quiet = Beeper::kRate;
    for (int16_t s : samples) {
        if (s != 0) {
            if (quiet >= Beeper::kRate * 5 / 1000) ++beeps;
            quiet = 0;
        } else {
            ++quiet;
        }
    }
    return beeps;
}

void test_beeper()
{
    // Two seconds at one beep per 200 ms, rendered in uneven chunks.
    Beeper beeper;
    std::vector<int16_t> samples(2 * Beeper::kRate);
    std::size_t done = 0;
    std::size_t chunk = 37;
    while (done < samples.size()) {
        const std::size_t n = std::min(chunk, samples.size() - done);
        beeper.render(samples.data() + done, n, 200);
        done += n;
        chunk = chunk * 3 % 411 + 1;
    }
    assert(count_beeps(samples) == 10);
    // The first beep starts at once and lasts kBeepSamples.
    assert(samples[0] == 0 && samples[4] != 0);
    for (int i = Beeper::kBeepSamples; i < Beeper::kRate / 5; ++i) assert(samples[static_cast<std::size_t>(i)] == 0);
    int loudest = 0;
    for (int16_t s : samples) loudest = std::max(loudest, std::abs(static_cast<int>(s)));
    assert(loudest > Beeper::kAmplitude * 9 / 10 && loudest <= Beeper::kAmplitude);

    // Faster for a stronger signal.
    Beeper fast;
    fast.render(samples.data(), samples.size(), 100);
    assert(count_beeps(samples) == 20);

    // Silence: a beep in progress finishes, then nothing.
    Beeper quiet;
    std::vector<int16_t> head(100);
    quiet.render(head.data(), head.size(), 500);
    assert(count_beeps(head) == 1);
    quiet.render(samples.data(), samples.size(), 0);
    assert(samples[0] != 0 || samples[1] != 0);
    for (std::size_t i = Beeper::kBeepSamples; i < samples.size(); ++i) assert(samples[i] == 0);

    Beeper never;
    never.render(samples.data(), samples.size(), 0);
    assert(count_beeps(samples) == 0);
}

void test_history()
{
    History history(4, 2);
    assert(history.columns().empty());
    assert(history.capacity() == 4);
    history.push(-80.0f);
    assert(history.columns().empty()); // half a column
    history.push(-70.0f);
    assert(history.columns().size() == 1 && near(history.columns()[0], -70.0f));
    history.push(-60.0f);
    history.push(-90.0f);
    assert(history.columns().size() == 2 && near(history.columns()[1], -60.0f));
    for (int i = 0; i < 6; ++i) history.push(-50.0f - static_cast<float>(i));
    // Full: the oldest column scrolled off, the newest is last.
    assert(history.columns().size() == 4);
    assert(near(history.columns()[0], -60.0f));
    assert(near(history.columns()[3], -54.0f));
    history.clear();
    assert(history.columns().empty());
    history.push(-40.0f);
    assert(history.columns().empty());

    assert(History(300, 2).seconds(25.0f) == 24);
    assert(History(300, 1).seconds(30.0f) == 10);
    assert(History(300, 2).seconds(0.0f) == 0);
}

} // namespace

int main()
{
    test_bar();
    test_frequencies();
    test_smoothing_and_peak();
    test_trend();
    test_zero();
    test_beep_interval();
    test_beeper();
    test_history();
    return 0;
}

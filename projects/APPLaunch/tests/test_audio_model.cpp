/*
 * SPDX-License-Identifier: MIT
 */

#include "../main/ui/model/audio_model.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

std::vector<int16_t> sine(float hz, float amplitude)
{
    std::vector<int16_t> out(audio::kFftSize);
    for (int i = 0; i < audio::kFftSize; ++i)
        out[i] = static_cast<int16_t>(amplitude * 32767.0f *
                                      std::sin(2.0f * 3.14159265f * hz * static_cast<float>(i) / audio::kSampleRate));
    return out;
}

void test_peak_frequency_is_sub_bin_accurate()
{
    for (float hz : {82.41f, 440.0f, 1000.0f, 3520.0f, 12000.0f}) {
        const std::vector<int16_t> samples = sine(hz, 0.5f);
        const std::vector<float> db = audio::spectrum_db(samples.data(), samples.size());
        const audio::Peak peak = audio::find_peak(db, audio::kSampleRate, audio::kMinHz, audio::kMaxHz, -60.0f);
        assert(peak.valid);
        assert(std::fabs(peak.hz - hz) < hz * 0.002f + 1.0f);
        // Half scale is -6 dBFS; Hann scalloping costs at most ~1.5 dB.
        assert(peak.db > -8.0f && peak.db < -5.0f);
    }
}

void test_silence_is_gated()
{
    const std::vector<int16_t> samples(audio::kFftSize, 0);
    const std::vector<float> db = audio::spectrum_db(samples.data(), samples.size());
    assert(!audio::find_peak(db, audio::kSampleRate, audio::kMinHz, audio::kMaxHz, -60.0f).valid);
    assert(audio::rms_dbfs(samples.data(), samples.size()) <= -99.0f);
}

void test_short_input_is_floor()
{
    const int16_t one = 1000;
    const std::vector<float> db = audio::spectrum_db(&one, 1);
    assert(db.size() == static_cast<std::size_t>(audio::kFftSize / 2));
    assert(db[10] == audio::kFloorDb);
}

void test_note_names()
{
    audio::Note a4 = audio::nearest_note(440.0f);
    assert(a4.name == "A4" && a4.cents == 0);
    audio::Note c4 = audio::nearest_note(261.63f);
    assert(c4.name == "C4" && std::abs(c4.cents) <= 1);
    audio::Note sharp = audio::nearest_note(445.0f);
    assert(sharp.name == "A4" && sharp.cents == 20);
    audio::Note e2 = audio::nearest_note(82.41f);
    assert(e2.name == "E2");
    audio::Note cs5 = audio::nearest_note(554.37f);
    assert(cs5.name == "C#5");
    assert(audio::nearest_note(0.0f).name.empty());
}

void test_log_bands_place_the_tone()
{
    const std::vector<int16_t> samples = sine(1000.0f, 0.5f);
    const std::vector<float> db = audio::spectrum_db(samples.data(), samples.size());
    const int bands = 64;
    const std::vector<float> out = audio::log_bands(db, audio::kSampleRate, bands, audio::kMinHz, audio::kMaxHz);
    assert(out.size() == static_cast<std::size_t>(bands));
    int loudest = 0;
    for (int i = 1; i < bands; ++i)
        if (out[i] > out[loudest]) loudest = i;
    const int expected = static_cast<int>(audio::log_position(1000.0f, audio::kMinHz, audio::kMaxHz) * bands);
    assert(std::abs(loudest - expected) <= 1);
}

void test_format_hz()
{
    assert(audio::format_hz(440.04f) == "440.0 Hz");
    assert(audio::format_hz(1000.24f) == "1000.2 Hz");
    assert(audio::format_hz(12345.0f) == "12.35 kHz");
}

} // namespace

int main()
{
    test_peak_frequency_is_sub_bin_accurate();
    test_silence_is_gated();
    test_short_input_is_floor();
    test_note_names();
    test_log_bands_place_the_tone();
    test_format_hz();
    std::puts("audio model: ok");
    return 0;
}

// SPDX-License-Identifier: MIT

#include "../main/ui/model/rec_model.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace rec;

namespace {

void put32(std::vector<uint8_t> &v, uint32_t x)
{
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

void put16(std::vector<uint8_t> &v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

void tag(std::vector<uint8_t> &v, const char *t)
{
    v.insert(v.end(), t, t + 4);
}

// Canonical header, optionally with a LIST chunk before "data".
std::vector<uint8_t> wav_header(uint16_t format, uint16_t channels, uint32_t rate, uint16_t bits, uint32_t data_size,
                                bool list_chunk)
{
    std::vector<uint8_t> v;
    tag(v, "RIFF");
    put32(v, 0);
    tag(v, "WAVE");
    tag(v, "fmt ");
    put32(v, 16);
    put16(v, format);
    put16(v, channels);
    put32(v, rate);
    put32(v, rate * channels * bits / 8);
    put16(v, static_cast<uint16_t>(channels * bits / 8));
    put16(v, bits);
    if (list_chunk) {
        tag(v, "LIST");
        put32(v, 5);
        v.insert(v.end(), {'I', 'N', 'F', 'O', 'x', 0});
    }
    tag(v, "data");
    put32(v, data_size);
    return v;
}

} // namespace

int main()
{
    assert(recording_stem(2026, 11, 5, 4, 45, 36) == "20261105_044536");
    assert(stem_label("20261105_044536") == "11-05 04:45");
    assert(stem_label("voice memo") == "voice memo");

    assert(format_duration(0) == "0:00");
    assert(format_duration(7400) == "0:07");
    assert(format_duration(65000) == "1:05");
    assert(format_duration(720000) == "12:00");

    // 48 kHz mono float: 2.825 s is 542400 data bytes.
    const std::vector<uint8_t> f32 = wav_header(3, 1, 48000, 32, 542400, false);
    assert(wav_duration_ms(f32.data(), f32.size(), f32.size() + 542400) == 2825);
    // 16 kHz mono 16-bit with a LIST chunk; data size field larger than the file.
    const std::vector<uint8_t> s16 = wav_header(1, 1, 16000, 16, 0x7FFFFFFF, true);
    assert(wav_duration_ms(s16.data(), s16.size(), s16.size() + 32000) == 1000);
    // Header-only file still being written.
    assert(wav_duration_ms(s16.data(), s16.size(), s16.size()) == 0);
    const uint8_t junk[16] = {'R', 'I', 'F', 'X'};
    assert(wav_duration_ms(junk, sizeof(junk), 1000) == 0);

    assert(clean_transcript(" 1, 2, 3.\n") == "1, 2, 3.");
    assert(clean_transcript(" Buy milk.\n And call the plumber.\n") == "Buy milk. And call the plumber.");
    assert(clean_transcript("[BLANK_AUDIO]\n") == "");
    assert(clean_transcript(" (wind blowing) Hello [ Silence ] there.\n") == "Hello there.");

    assert(clip_text("hello world", 5) == "hello");
    assert(clip_text("hi", 5) == "hi");
    assert(clip_text("caf\xC3\xA9 ok", 4) == "caf\xC3\xA9");

    std::vector<uint8_t> silence(64, 0);
    assert(peak_level(silence.data(), silence.size()) == 0);
    const std::vector<uint8_t> full = {0x00, 0x80, 0xFF, 0x7F};
    assert(peak_level(full.data(), full.size()) == 100);
    // -20 dBFS (3277) sits two thirds of the way up the -60..0 scale.
    const std::vector<uint8_t> quiet = {0xCD, 0x0C};
    assert(peak_level(quiet.data(), quiet.size()) == 67);

    assert(transcribe_estimate_ms(0) == 3000);
    assert(transcribe_estimate_ms(4000) == 8000);
    return 0;
}

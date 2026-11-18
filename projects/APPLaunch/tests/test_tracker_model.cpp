// SPDX-License-Identifier: MIT

#include "../main/ui/model/tracker_model.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace tracker;

namespace {

uint32_t le32(const std::vector<uint8_t> &b, std::size_t at)
{
    return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
           (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}

uint32_t le16(const std::vector<uint8_t> &b, std::size_t at)
{
    return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8);
}

void test_notes()
{
    assert(std::abs(note_frequency(kNoteA4) - 440.0) < 1e-9);
    assert(std::abs(note_frequency(parse_note("C-4")) - 261.6256) < 0.001);
    assert(std::abs(note_frequency(parse_note("A-5")) - 880.0) < 1e-6);
    assert(std::abs(note_frequency(parse_note("C-0")) - 16.3516) < 0.001);
    assert(parse_note("C-0") == kMinNote);
    assert(parse_note("B-7") == kMaxNote);
    assert(parse_note("A-4") == kNoteA4);
    assert(note_name(kNoteA4) == "A-4");
    assert(note_name(parse_note("D#5")) == "D#5");
    assert(note_name(kEmpty) == "---");
    assert(note_name(kOff) == "===");
    assert(parse_note("H-4") == 0);
    assert(parse_note("C-8") == 0);
    assert(parse_note("C4") == 0);
    for (int note = kMinNote; note <= kMaxNote; ++note) assert(parse_note(note_name(note)) == note);
    for (int note = kMinNote; note + 12 <= kMaxNote; ++note)
        assert(std::abs(note_frequency(note + 12) / note_frequency(note) - 2.0) < 1e-9);

    Cell cell;
    assert(cell_text(cell) == "--- --");
    cell.note = kOff;
    assert(cell_text(cell) == "=== --");
    cell = Cell{static_cast<uint8_t>(parse_note("C-4")), 3, 10};
    assert(cell_text(cell) == "C-4 A4");
}

struct SquareStats {
    int rising = 0;
    double high_fraction = 0.0;
    double period = 0.0;
    int low = 0;
    int high = 0;
};

SquareStats square_stats(int duty, int note)
{
    Synth synth;
    synth.note_on(kSquare1, note, duty, kMaxVolume, 0);
    std::vector<int16_t> out(kRate);
    synth.render(out.data(), kRate);
    SquareStats stats;
    int highs = 0;
    int first = -1;
    int last = -1;
    stats.low = *std::min_element(out.begin(), out.end());
    stats.high = *std::max_element(out.begin(), out.end());
    for (int i = 0; i < kRate; ++i) {
        assert(out[i] == kChannelPeak || out[i] == -kChannelPeak);
        if (out[i] > 0) ++highs;
        if (i > 0 && out[i] > 0 && out[i - 1] < 0) {
            ++stats.rising;
            if (first < 0) first = i;
            last = i;
        }
    }
    stats.high_fraction = static_cast<double>(highs) / kRate;
    stats.period = static_cast<double>(last - first) / (stats.rising - 1);
    return stats;
}

void test_square()
{
    static constexpr double kDuties[4] = {0.125, 0.25, 0.5, 0.75};
    for (int duty = 0; duty < 4; ++duty) {
        const SquareStats a4 = square_stats(duty, kNoteA4);
        // 440 Hz for one second: 440 cycles (the first edge is at sample 0 and not counted).
        assert(a4.rising >= 439 && a4.rising <= 440);
        assert(std::abs(a4.period - kRate / 440.0) < 0.05);
        assert(std::abs(a4.high_fraction - kDuties[duty]) < 0.005);
        assert(a4.high == kChannelPeak && a4.low == -kChannelPeak);
    }
    const SquareStats c4 = square_stats(2, parse_note("C-4"));
    assert(std::abs(c4.period - kRate / 261.6256) < 0.05);
}

void test_volume_envelope_and_levels()
{
    Synth synth;
    std::vector<int16_t> out(kRate);
    synth.note_on(kSquare2, kNoteA4, 2, 8, 0);
    synth.render(out.data(), 1000);
    assert(std::abs(out[0]) == 8 * kChannelPeak / kMaxVolume);
    assert(synth.level(kSquare1) == 0.0f);
    assert(std::abs(synth.level(kSquare2) - 8.0f / 15.0f) < 0.001f);
    assert(!synth.silent());

    // Envelope 1: one volume step every 1/64 s, so 15 steps are over after 7500 samples.
    synth.note_on(kSquare1, kNoteA4, 2, kMaxVolume, 1);
    synth.note_off(kSquare2);
    synth.render(out.data(), kRate);
    assert(std::abs(out[0]) == kChannelPeak);
    assert(std::abs(out[kRate / 64 - 1]) == kChannelPeak);
    assert(std::abs(out[kRate / 64]) == 14 * kChannelPeak / kMaxVolume);
    assert(out[7499] != 0);
    for (int i = 7500; i < kRate; ++i) assert(out[i] == 0);
    assert(synth.silent());

    // A gated note stops after its gate.
    synth.note_on(kWave, kNoteA4, 0, kMaxVolume, 0, 100);
    synth.render(out.data(), 400);
    bool any = false;
    for (int i = 0; i < 100; ++i) any = any || out[i] != 0;
    assert(any);
    for (int i = 100; i < 400; ++i) assert(out[i] == 0);

    // The wave channel plays the 32-sample table once per period.
    for (int w = 0; w < kInstrumentCount[kWave]; ++w)
        for (uint8_t v : wave_table(w)) assert(v <= 15);
    assert(wave_table(0)[0] == 0 && wave_table(0)[15] == 15 && wave_table(0)[16] == 15 && wave_table(0)[31] == 0);
    synth.note_on(kWave, kNoteA4, 0, kMaxVolume, 0);
    synth.render(out.data(), kRate);
    int rising = 0;
    for (int i = 1; i < kRate; ++i)
        if (out[i] > 0 && out[i - 1] <= 0) ++rising;
    assert(rising >= 439 && rising <= 441);
    assert(*std::max_element(out.begin(), out.end()) == kChannelPeak);
}

void test_silence()
{
    Synth synth;
    std::vector<int16_t> out(4096, 123);
    synth.render(out.data(), 4096);
    for (int16_t s : out) assert(s == 0);
    assert(synth.silent());

    Song empty;
    Player player;
    out.assign(20000, 123);
    player.render(empty, out.data(), 20000);
    for (int16_t s : out) assert(s == 0);
    player.play(empty, 0);
    out.assign(20000, 123);
    player.render(empty, out.data(), 20000);
    for (int16_t s : out) assert(s == 0);
    assert(player.playing());
    for (int c = 0; c < kChannels; ++c) assert(player.level(c) == 0.0f);
}

void test_noise()
{
    std::vector<int16_t> a(8000), b(8000), c(8000);
    Synth s1, s2;
    s1.note_on(kNoise, parse_note("C-5"), 0, kMaxVolume, 0);
    s2.note_on(kNoise, parse_note("C-5"), 0, kMaxVolume, 0);
    s1.render(a.data(), 8000);
    s2.render(b.data(), 8000);
    assert(a == b);
    // Retriggering restarts the LFSR, so the same note sounds the same again.
    s1.note_on(kNoise, parse_note("C-5"), 0, kMaxVolume, 0);
    s1.render(b.data(), 8000);
    assert(a == b);
    // The 7-bit mode is a different sequence that repeats every 127 shifts.
    s1.note_on(kNoise, parse_note("C-5"), 1, kMaxVolume, 0);
    s1.render(c.data(), 8000);
    assert(a != c);
    int flips = 0;
    for (int i = 1; i < 8000; ++i) {
        assert(std::abs(a[i]) == kChannelPeak);
        if (a[i] != a[i - 1]) ++flips;
    }
    // 0.25 s at a 4186 Hz clock is 1046 shifts; about half of them flip the output.
    assert(flips > 400 && flips < 650);
    Synth s3;
    const int b6 = parse_note("B-6");
    assert(std::abs(note_frequency(b6) - 1975.53) < 0.01);
    s3.note_on(kNoise, b6, 1, kMaxVolume, 0);
    std::vector<int16_t> d(8000);
    s3.render(d.data(), 8000);
    int transitions = 0;
    for (int i = 1; i < 8000; ++i)
        if (d[i] != d[i - 1]) ++transitions;
    // A maximal 7-bit LFSR has 64 output transitions per 127-shift period.
    const double shifts = 8000.0 * note_frequency(b6) * 8.0 / kRate;
    assert(std::abs(transitions - shifts * 64.0 / 127.0) < 8.0);
}

void test_tempo()
{
    assert(samples_per_row(120) == 4000);
    assert(samples_per_row(150) == 3200);
    assert(samples_per_row(60) == 8000);
    assert(clamp_bpm(1) == kMinBpm);
    assert(clamp_bpm(1000) == kMaxBpm);

    Song song;
    set_note(song, 0, 0, kSquare1, kNoteA4, 2, kMaxVolume);
    Player player;
    std::vector<int16_t> out(48000);
    player.play(song, 0);
    assert(player.row() == 0);
    player.render(song, out.data(), 3999);
    assert(player.row() == 0);
    player.render(song, out.data(), 1);
    assert(player.row() == 1);
    player.render(song, out.data(), 4000);
    assert(player.row() == 2);

    // 130 BPM is 3692.3 samples per row; the remainder carries, so 13 rows
    // are exactly 48000 samples.
    song.bpm = 130;
    assert(samples_per_row(130) == 3692);
    player.play(song, 0);
    player.render(song, out.data(), 47999);
    assert(player.row() == 12);
    player.render(song, out.data(), 1);
    assert(player.row() == 13);
}

void test_order_wrap()
{
    Song song;
    song.order = {2, 0};
    set_note(song, 0, 0, kSquare1, kNoteA4, 2, kMaxVolume);
    set_note(song, 2, 0, kWave, kNoteA4, 0, kMaxVolume);
    Player player;
    std::vector<int16_t> out(4000);
    player.play(song, 2);
    assert(player.order_position() == 0 && player.pattern() == 2 && player.row() == 0);
    for (int i = 0; i < kRows; ++i) player.render(song, out.data(), 4000);
    assert(player.order_position() == 1 && player.pattern() == 0 && player.row() == 0);
    for (int i = 0; i < kRows - 1; ++i) player.render(song, out.data(), 4000);
    assert(player.order_position() == 1 && player.row() == kRows - 1);
    player.render(song, out.data(), 4000);
    assert(player.playing());
    assert(player.order_position() == 0 && player.pattern() == 2 && player.row() == 0);

    // Starting from the second entry continues from there.
    player.play(song, 0);
    assert(player.order_position() == 1);
    // A pattern outside the order list loops on its own.
    player.play(song, 5);
    assert(player.order_position() == -1 && player.pattern() == 5);
    for (int i = 0; i < kRows; ++i) player.render(song, out.data(), 4000);
    assert(player.playing() && player.pattern() == 5 && player.row() == 0);

    // Without loop the song stops after the last row.
    player.play_song(song, false);
    for (int i = 0; i < 2 * kRows; ++i) {
        assert(player.playing());
        player.render(song, out.data(), 4000);
    }
    assert(!player.playing());

    // Stop silences the voices.
    player.play(song, 0);
    player.render(song, out.data(), 100);
    assert(out[50] != 0);
    player.stop();
    player.render(song, out.data(), 100);
    for (int i = 0; i < 100; ++i) assert(out[i] == 0);
    assert(player.silent());

    // A preview sounds while stopped and releases by itself.
    Cell cell{static_cast<uint8_t>(kNoteA4), 2, 12};
    player.preview(kSquare2, cell, 0);
    player.render(song, out.data(), 100);
    assert(std::abs(out[0]) == 12 * kChannelPeak / kMaxVolume);
    assert(!player.silent());
    std::vector<int16_t> longer(kRate);
    player.render(song, longer.data(), kRate);
    assert(player.silent());
    assert(longer[kRate - 1] == 0);
}

void test_editing()
{
    Song song;
    assert(set_note(song, 1, 3, kSquare1, parse_note("C-4"), 9, 99));
    const Cell &cell = song.patterns[1].rows[3][kSquare1];
    assert(cell.note == parse_note("C-4") && cell.inst == 3 && cell.vol == kMaxVolume);
    assert(set_note(song, 1, 3, kNoise, parse_note("C-4"), 9, 5));
    assert(song.patterns[1].rows[3][kNoise].inst == 1);
    assert(!set_note(song, kMaxPatterns, 0, 0, kNoteA4, 0, 0));
    assert(!set_note(song, 0, kRows, 0, kNoteA4, 0, 0));
    assert(!set_note(song, 0, 0, kChannels, kNoteA4, 0, 0));
    assert(!set_note(song, 0, 0, 0, 0, 0, 0));
    assert(!set_note(song, 0, 0, 0, kMaxNote + 1, 0, 0));

    assert(transpose_cell(song, 1, 3, kSquare1, 2));
    assert(note_name(cell.note) == "D-4");
    assert(transpose_cell(song, 1, 3, kSquare1, -500));
    assert(cell.note == kMinNote);
    assert(transpose_cell(song, 1, 3, kSquare1, 500));
    assert(cell.note == kMaxNote);
    assert(!transpose_cell(song, 1, 4, kSquare1, 1));
    assert(set_note_off(song, 1, 4, kSquare1));
    assert(song.patterns[1].rows[4][kSquare1].note == kOff);
    assert(!transpose_cell(song, 1, 4, kSquare1, 1));

    // Insert pushes the channel down and leaves the others alone.
    assert(insert_row(song, 1, 3, kSquare1));
    assert(song.patterns[1].rows[3][kSquare1].note == kEmpty);
    assert(song.patterns[1].rows[4][kSquare1].note == kMaxNote);
    assert(song.patterns[1].rows[5][kSquare1].note == kOff);
    assert(song.patterns[1].rows[3][kNoise].note == parse_note("C-4"));
    assert(delete_row(song, 1, 3, kSquare1));
    assert(song.patterns[1].rows[3][kSquare1].note == kMaxNote);
    assert(song.patterns[1].rows[4][kSquare1].note == kOff);
    // The last row falls off on insert.
    assert(set_note(song, 1, kRows - 1, kWave, kNoteA4, 0, 8));
    assert(insert_row(song, 1, 0, kWave));
    for (int r = 0; r < kRows; ++r) assert(song.patterns[1].rows[r][kWave].note == kEmpty);

    assert(clear_cell(song, 1, 3, kSquare1));
    assert(clear_cell(song, 1, 4, kSquare1));
    assert(clear_cell(song, 1, 3, kNoise));
    assert(pattern_empty(song.patterns[1]));

    assert(song.order.size() == 1);
    assert(!pop_order(song));
    assert(append_order(song, 3));
    assert(!append_order(song, kMaxPatterns));
    assert(song.order.size() == 2 && song.order[1] == 3);
    assert(pop_order(song));
    while (song.order.size() < static_cast<std::size_t>(kMaxOrder)) assert(append_order(song, 1));
    assert(!append_order(song, 1));
}

void test_text_round_trip()
{
    Song song = demo_song();
    assert(song.bpm == 126);
    assert(song.order.size() == 2);
    assert(!pattern_empty(song.patterns[0]) && !pattern_empty(song.patterns[1]));
    assert(pattern_empty(song.patterns[2]));
    assert(cell_text(song.patterns[0].rows[0][kSquare1]) == "C-5 C3");
    assert(cell_text(song.patterns[0].rows[6][kWave]) == "=== --");
    assert(cell_text(song.patterns[1].rows[15][kNoise]) == "C-5 C1");

    song.bpm = 173;
    song.envelope = {0, 7, 3, 1};
    song.order = {1, 0, 15, 1};
    set_note(song, 15, 15, kNoise, kMaxNote, 1, 0);
    set_note(song, 15, 0, kSquare1, kMinNote, 0, 15);
    set_note_off(song, 7, 9, kWave);
    const std::string text = to_text(song);
    Song loaded;
    assert(from_text(text, loaded));
    assert(loaded == song);
    assert(to_text(loaded) == text);
    assert(text.find("F --- -- | --- -- | --- -- | B-7 02\n") != std::string::npos);
    assert(text.find("order 1 0 15 1\n") != std::string::npos);

    // Comments, blank lines and CRLF are accepted.
    Song crlf;
    assert(from_text("# my song\r\n\r\ncp0 tracker song 1\r\nbpm 90\r\npattern 2\r\n"
                     "3 A-4 F3 | --- -- | --- -- | === --\r\n",
                     crlf));
    assert(crlf.bpm == 90);
    assert(crlf.patterns[2].rows[3][kSquare1].note == kNoteA4);
    assert(crlf.patterns[2].rows[3][kNoise].note == kOff);

    // Bad input leaves the song untouched.
    Song keep = song;
    assert(!from_text("", keep));
    assert(!from_text("not a song\nbpm 100\n", keep));
    assert(!from_text("cp0 tracker song 1\nbpm 9999\n", keep));
    assert(!from_text("cp0 tracker song 1\norder 16\n", keep));
    assert(!from_text("cp0 tracker song 1\n0 C-4 F1 | --- -- | --- -- | --- --\n", keep));
    assert(!from_text("cp0 tracker song 1\npattern 0\n0 C-4 F1 | --- -- | --- --\n", keep));
    assert(!from_text("cp0 tracker song 1\npattern 0\n0 C-4 F5 | --- -- | --- -- | --- --\n", keep));
    assert(!from_text("cp0 tracker song 1\npattern 0\n0 --- -- | --- -- | --- -- | C-4 F3\n", keep));
    assert(!from_text("cp0 tracker song 1\npattern 0\n0 X-4 F1 | --- -- | --- -- | --- --\n", keep));
    assert(!from_text("cp0 tracker song 1\nenv 1 2 3\n", keep));
    assert(keep == song);
}

void test_wav()
{
    Song song;
    set_note(song, 0, 0, kSquare1, kNoteA4, 2, kMaxVolume);
    const std::vector<int16_t> pcm = render_song(song);
    // 16 rows of 4000 samples plus the quarter-second tail.
    assert(pcm.size() == static_cast<std::size_t>(16 * 4000 + kRate / 4));
    assert(std::abs(pcm[0]) == kChannelPeak);
    assert(pcm.back() == 0);

    const std::vector<uint8_t> wav = wav_bytes(pcm);
    assert(wav.size() == 44 + pcm.size() * 2);
    assert(std::string(wav.begin(), wav.begin() + 4) == "RIFF");
    assert(le32(wav, 4) == wav.size() - 8);
    assert(std::string(wav.begin() + 8, wav.begin() + 16) == "WAVEfmt ");
    assert(le32(wav, 16) == 16);
    assert(le16(wav, 20) == 1);
    assert(le16(wav, 22) == 1);
    assert(le32(wav, 24) == 32000);
    assert(le32(wav, 28) == 64000);
    assert(le16(wav, 32) == 2);
    assert(le16(wav, 34) == 16);
    assert(std::string(wav.begin() + 36, wav.begin() + 40) == "data");
    assert(le32(wav, 40) == pcm.size() * 2);
    assert(static_cast<int16_t>(le16(wav, 44)) == pcm[0]);
    assert(static_cast<int16_t>(le16(wav, 44 + 2 * 100)) == pcm[100]);

    // The demo makes sound on all four channels and never clips.
    const Song demo = demo_song();
    const std::vector<int16_t> demo_pcm = render_song(demo);
    assert(demo_pcm.size() == static_cast<std::size_t>(32LL * kRate * 60 / (126 * 4) + kRate / 4));
    int peak = 0;
    for (int16_t s : demo_pcm) peak = std::max(peak, std::abs(static_cast<int>(s)));
    assert(peak > kChannelPeak && peak <= 4 * kChannelPeak);
    Player player;
    player.play_song(demo, true);
    std::vector<int16_t> block(1000);
    player.render(demo, block.data(), 1000);
    for (int c = 0; c < kChannels; ++c) assert(player.level(c) > 0.3f);
}

} // namespace

int main()
{
    test_notes();
    test_square();
    test_volume_envelope_and_levels();
    test_silence();
    test_noise();
    test_tempo();
    test_order_wrap();
    test_editing();
    test_text_round_trip();
    test_wav();
    std::printf("test_tracker_model: ok\n");
    return 0;
}

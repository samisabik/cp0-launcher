/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Four-channel chiptune tracker modelled on the Game Boy sound channels: two
// squares, a 32-sample 4-bit wavetable and LFSR noise. Song data, a text
// format, a sample-accurate synth and player, and WAV export.
namespace tracker {

inline constexpr int kRate = 32000;
inline constexpr int kRows = 16;
inline constexpr int kChannels = 4;
inline constexpr int kMaxPatterns = 16;
inline constexpr int kMaxOrder = 64;
inline constexpr int kRowsPerBeat = 4;
inline constexpr int kMinBpm = 40;
inline constexpr int kMaxBpm = 300;
inline constexpr int kMaxVolume = 15;
inline constexpr int kMaxEnvelope = 7;
// Peak sample value of one channel at full volume; four channels fit in 16 bits.
inline constexpr int kChannelPeak = 6000;

// Notes are numbered from 1 (C-0) to 96 (B-7); A-4 is 58.
inline constexpr uint8_t kEmpty = 0;
inline constexpr uint8_t kOff = 255;
inline constexpr int kMinNote = 1;
inline constexpr int kMaxNote = 96;
inline constexpr int kNoteA4 = 58;

enum Channel { kSquare1 = 0, kSquare2 = 1, kWave = 2, kNoise = 3 };
// Square: duty 12.5 / 25 / 50 / 75 %. Wave: built-in table. Noise: LFSR width.
inline constexpr std::array<int, kChannels> kInstrumentCount = {4, 4, 4, 2};

struct Cell {
    uint8_t note = kEmpty;
    uint8_t inst = 0;
    uint8_t vol = 0;

    bool operator==(const Cell &other) const
    {
        return note == other.note && inst == other.inst && vol == other.vol;
    }
};

struct Pattern {
    std::array<std::array<Cell, kChannels>, kRows> rows{};

    bool operator==(const Pattern &other) const { return rows == other.rows; }
};

struct Song {
    int bpm = 120;
    // Per channel: the volume drops one step every n/64 s; 0 holds the note.
    std::array<uint8_t, kChannels> envelope = {2, 2, 0, 1};
    std::vector<uint8_t> order = {0};
    std::array<Pattern, kMaxPatterns> patterns{};

    bool operator==(const Song &other) const
    {
        return bpm == other.bpm && envelope == other.envelope && order == other.order && patterns == other.patterns;
    }
};

double note_frequency(int note);
// "C-4", "D#5"; "---" for empty and "===" for note-off.
std::string note_name(int note);
// Returns 0 when the text is not a note name.
int parse_note(const std::string &text);
// "C-4 A3": note, volume as a hex digit, instrument counted from 1.
std::string cell_text(const Cell &cell);
const char *channel_name(int channel);
const char *instrument_name(int channel, int inst);
int clamp_bpm(int bpm);
const std::array<uint8_t, 32> &wave_table(int index);

bool valid_position(int pattern, int row, int channel);
bool set_note(Song &song, int pattern, int row, int channel, int note, int inst, int vol);
bool set_note_off(Song &song, int pattern, int row, int channel);
bool clear_cell(Song &song, int pattern, int row, int channel);
// Moves a note by semitones, clamped to the note range. False for empty and note-off cells.
bool transpose_cell(Song &song, int pattern, int row, int channel, int semitones);
// Shifts one channel down from the row (the last row falls off) or up onto it.
bool insert_row(Song &song, int pattern, int row, int channel);
bool delete_row(Song &song, int pattern, int row, int channel);
bool pattern_empty(const Pattern &pattern);
bool append_order(Song &song, int pattern);
// Keeps at least one entry.
bool pop_order(Song &song);

std::string to_text(const Song &song);
// Leaves the song untouched and returns false when the text is not a valid song.
bool from_text(const std::string &text, Song &song);
Song demo_song();

class Synth
{
public:
    Synth();
    // gate_samples > 0 releases the note after that many samples.
    void note_on(int channel, int note, int inst, int vol, int envelope, int gate_samples = 0);
    void note_off(int channel);
    void all_off();
    bool silent() const;
    // Peak of the channel in the last render call, 0..1.
    float level(int channel) const { return level_[static_cast<std::size_t>(channel)]; }
    void render(int16_t *out, int frames);

private:
    struct Voice {
        bool on = false;
        int inst = 0;
        int vol = 0;
        uint32_t phase = 0;
        uint32_t step = 0;
        uint64_t noise_phase = 0;
        uint64_t noise_step = 0;
        uint16_t lfsr = 0x7FFF;
        int env_period = 0;
        int env_count = 0;
        int gate = 0;
    };

    std::array<Voice, kChannels> voices_{};
    std::array<float, kChannels> level_{};
    std::vector<int32_t> mix_;
};

class Player
{
public:
    // Starts at the top of the pattern. When the pattern is in the order list
    // playback follows the order from its first occurrence and wraps to the
    // start; otherwise the pattern loops on its own.
    void play(const Song &song, int pattern);
    // Starts at the first order entry; without loop, playback stops after the last row.
    void play_song(const Song &song, bool loop);
    void stop();
    void preview(int channel, const Cell &cell, int envelope);

    bool playing() const { return playing_; }
    // -1 while a pattern outside the order list loops.
    int order_position() const { return order_pos_; }
    int pattern() const { return pattern_; }
    int row() const { return row_; }
    bool silent() const { return !playing_ && synth_.silent(); }
    float level(int channel) const { return synth_.level(channel); }

    void render(const Song &song, int16_t *out, int frames);

private:
    Synth synth_;
    bool playing_ = false;
    bool loop_ = true;
    bool fresh_ = false;
    int order_pos_ = -1;
    int pattern_ = 0;
    int row_ = 0;
    int remaining_ = 0;
    int carry_ = 0;

    void begin_row(const Song &song);
    void advance(const Song &song);
};

// The order list played once, plus a short tail for the last notes to decay.
std::vector<int16_t> render_song(const Song &song);
// A complete RIFF/WAVE file: 44-byte header and 16-bit mono PCM.
std::vector<uint8_t> wav_bytes(const std::vector<int16_t> &samples, int rate = kRate);

} // namespace tracker

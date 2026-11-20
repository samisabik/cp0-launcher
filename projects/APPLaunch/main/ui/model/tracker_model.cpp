/*
 * SPDX-License-Identifier: MIT
 */

#include "tracker_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tracker {

namespace {

constexpr const char *kHeader = "cp0 tracker song 1";
constexpr const char *kNoteNames[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
constexpr int kPreviewSamples = kRate / 4;
constexpr int kTailSamples = kRate / 4;

constexpr const char *kDemo =
    "cp0 tracker song 1\n"
    "bpm 126\n"
    "env 2 3 0 1\n"
    "order 0 1\n"
    "pattern 0\n"
    "0 C-5 C3 | G-4 72 | C-3 F1 | C-2 F2\n"
    "2 D#5 C3 | --- -- | --- -- | C-7 61\n"
    "4 G-5 C3 | D#4 72 | C-3 F1 | C-5 C1\n"
    "6 D#5 C3 | --- -- | === -- | C-7 61\n"
    "8 C-5 C3 | G#4 72 | G#2 F1 | C-2 F2\n"
    "A G#4 C3 | --- -- | --- -- | C-7 61\n"
    "C A#4 C3 | F-4 72 | A#2 F1 | C-5 C1\n"
    "E D-5 C3 | --- -- | === -- | C-7 61\n"
    "F --- -- | --- -- | --- -- | C-7 41\n"
    "pattern 1\n"
    "0 D#5 C3 | G-4 72 | C-3 F1 | C-2 F2\n"
    "2 G-5 C3 | --- -- | --- -- | C-7 61\n"
    "3 D#5 A3 | --- -- | --- -- | --- --\n"
    "4 C-6 C3 | D#4 72 | C-3 F1 | C-5 C1\n"
    "6 G-5 C3 | --- -- | === -- | C-7 61\n"
    "8 G#5 C3 | C-5 72 | G#2 F1 | C-2 F2\n"
    "A D#5 C3 | --- -- | --- -- | C-7 61\n"
    "C F-5 C3 | D-5 72 | A#2 F1 | C-5 C1\n"
    "D D-5 A3 | --- -- | --- -- | --- --\n"
    "E A#4 C3 | --- -- | === -- | C-5 A1\n"
    "F D-5 C3 | --- -- | --- -- | C-5 C1\n";

std::string trim(const std::string &text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) --end;
    return text.substr(begin, end - begin);
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Decimal integers separated by spaces; false on anything else.
bool parse_ints(const std::string &text, std::vector<int> &out)
{
    out.clear();
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == ' ' || text[i] == '\t') {
            ++i;
            continue;
        }
        int value = 0;
        int digits = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 6) {
            value = value * 10 + (text[i] - '0');
            ++i;
            ++digits;
        }
        if (digits == 0 || (i < text.size() && text[i] != ' ' && text[i] != '\t')) return false;
        out.push_back(value);
    }
    return true;
}

bool parse_cell(const std::string &raw, int channel, Cell &cell)
{
    const std::string text = trim(raw);
    if (text.size() != 6 || text[3] != ' ') return false;
    const std::string note = text.substr(0, 3);
    const std::string tail = text.substr(4, 2);
    cell = Cell{};
    if (note == "---") return tail == "--";
    if (note == "===") {
        cell.note = kOff;
        return tail == "--";
    }
    const int number = parse_note(note);
    const int vol = hex_value(tail[0]);
    const int inst = tail[1] - '1';
    if (number == 0 || vol < 0 || inst < 0 || inst >= kInstrumentCount[static_cast<std::size_t>(channel)]) return false;
    cell.note = static_cast<uint8_t>(number);
    cell.vol = static_cast<uint8_t>(vol);
    cell.inst = static_cast<uint8_t>(inst);
    return true;
}

bool parse_row(const std::string &line, Pattern &pattern)
{
    const int row = hex_value(line[0]);
    if (row < 0 || row >= kRows) return false;
    std::size_t start = 1;
    for (int channel = 0; channel < kChannels; ++channel) {
        const bool last = channel == kChannels - 1;
        const std::size_t bar = line.find('|', start);
        if (last != (bar == std::string::npos)) return false;
        const std::string part = last ? line.substr(start) : line.substr(start, bar - start);
        if (!parse_cell(part, channel, pattern.rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(channel)]))
            return false;
        start = bar + 1;
    }
    return true;
}

Cell &cell_at(Song &song, int pattern, int row, int channel)
{
    return song.patterns[static_cast<std::size_t>(pattern)]
        .rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(channel)];
}

uint32_t phase_step(double hz)
{
    return static_cast<uint32_t>(hz * 4294967296.0 / kRate);
}

void put_le(std::vector<uint8_t> &out, uint32_t value, int bytes)
{
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFFu));
}

} // namespace

double note_frequency(int note)
{
    return 440.0 * std::pow(2.0, (note - kNoteA4) / 12.0);
}

std::string note_name(int note)
{
    if (note == kOff) return "===";
    if (note < kMinNote || note > kMaxNote) return "---";
    const int index = note - 1;
    return std::string(kNoteNames[index % 12]) + static_cast<char>('0' + index / 12);
}

int parse_note(const std::string &text)
{
    if (text.size() != 3 || text[2] < '0' || text[2] > '7') return 0;
    for (int i = 0; i < 12; ++i)
        if (text[0] == kNoteNames[i][0] && text[1] == kNoteNames[i][1]) return (text[2] - '0') * 12 + i + 1;
    return 0;
}

std::string cell_text(const Cell &cell)
{
    if (cell.note == kEmpty) return "--- --";
    if (cell.note == kOff) return "=== --";
    std::string text = note_name(cell.note) + " ";
    text += "0123456789ABCDEF"[cell.vol & 15];
    text += static_cast<char>('1' + cell.inst);
    return text;
}

const char *channel_name(int channel)
{
    static constexpr const char *kNames[kChannels] = {"sq1", "sq2", "wav", "noi"};
    return channel >= 0 && channel < kChannels ? kNames[channel] : "";
}

const char *instrument_name(int channel, int inst)
{
    static constexpr const char *kDuty[4] = {"duty 12", "duty 25", "duty 50", "duty 75"};
    static constexpr const char *kWaves[4] = {"triangle", "saw", "sine", "organ"};
    static constexpr const char *kNoises[2] = {"noise 15", "noise 7"};
    if (channel < 0 || channel >= kChannels || inst < 0 || inst >= kInstrumentCount[static_cast<std::size_t>(channel)])
        return "";
    if (channel == kWave) return kWaves[inst];
    if (channel == kNoise) return kNoises[inst];
    return kDuty[inst];
}

int clamp_bpm(int bpm)
{
    return std::clamp(bpm, kMinBpm, kMaxBpm);
}

const std::array<uint8_t, 32> &wave_table(int index)
{
    static const std::array<std::array<uint8_t, 32>, 4> tables = [] {
        std::array<std::array<uint8_t, 32>, 4> t{};
        for (int i = 0; i < 32; ++i) {
            const double phase = 6.283185307179586 * (i + 0.5) / 32.0;
            t[0][static_cast<std::size_t>(i)] = static_cast<uint8_t>(i < 16 ? i : 31 - i);
            t[1][static_cast<std::size_t>(i)] = static_cast<uint8_t>(i / 2);
            t[2][static_cast<std::size_t>(i)] = static_cast<uint8_t>(std::lround(7.5 + 7.5 * std::sin(phase)));
            // Fundamental plus second and fourth harmonics.
            const double organ = std::sin(phase) + 0.5 * std::sin(2 * phase) + 0.25 * std::sin(4 * phase);
            t[3][static_cast<std::size_t>(i)] =
                static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(7.5 + 5.0 * organ)), 0, 15));
        }
        return t;
    }();
    return tables[static_cast<std::size_t>(std::clamp(index, 0, 3))];
}

bool valid_position(int pattern, int row, int channel)
{
    return pattern >= 0 && pattern < kMaxPatterns && row >= 0 && row < kRows && channel >= 0 && channel < kChannels;
}

bool set_note(Song &song, int pattern, int row, int channel, int note, int inst, int vol)
{
    if (!valid_position(pattern, row, channel) || note < kMinNote || note > kMaxNote) return false;
    Cell &cell = cell_at(song, pattern, row, channel);
    cell.note = static_cast<uint8_t>(note);
    cell.inst = static_cast<uint8_t>(std::clamp(inst, 0, kInstrumentCount[static_cast<std::size_t>(channel)] - 1));
    cell.vol = static_cast<uint8_t>(std::clamp(vol, 0, kMaxVolume));
    return true;
}

bool set_note_off(Song &song, int pattern, int row, int channel)
{
    if (!valid_position(pattern, row, channel)) return false;
    cell_at(song, pattern, row, channel) = Cell{kOff, 0, 0};
    return true;
}

bool clear_cell(Song &song, int pattern, int row, int channel)
{
    if (!valid_position(pattern, row, channel)) return false;
    cell_at(song, pattern, row, channel) = Cell{};
    return true;
}

bool transpose_cell(Song &song, int pattern, int row, int channel, int semitones)
{
    if (!valid_position(pattern, row, channel)) return false;
    Cell &cell = cell_at(song, pattern, row, channel);
    if (cell.note == kEmpty || cell.note == kOff) return false;
    cell.note = static_cast<uint8_t>(std::clamp(cell.note + semitones, kMinNote, kMaxNote));
    return true;
}

bool insert_row(Song &song, int pattern, int row, int channel)
{
    if (!valid_position(pattern, row, channel)) return false;
    for (int r = kRows - 1; r > row; --r) cell_at(song, pattern, r, channel) = cell_at(song, pattern, r - 1, channel);
    cell_at(song, pattern, row, channel) = Cell{};
    return true;
}

bool delete_row(Song &song, int pattern, int row, int channel)
{
    if (!valid_position(pattern, row, channel)) return false;
    for (int r = row; r < kRows - 1; ++r) cell_at(song, pattern, r, channel) = cell_at(song, pattern, r + 1, channel);
    cell_at(song, pattern, kRows - 1, channel) = Cell{};
    return true;
}

bool pattern_empty(const Pattern &pattern)
{
    for (const auto &row : pattern.rows)
        for (const Cell &cell : row)
            if (cell.note != kEmpty) return false;
    return true;
}

bool append_order(Song &song, int pattern)
{
    if (pattern < 0 || pattern >= kMaxPatterns || song.order.size() >= static_cast<std::size_t>(kMaxOrder))
        return false;
    song.order.push_back(static_cast<uint8_t>(pattern));
    return true;
}

bool pop_order(Song &song)
{
    if (song.order.size() <= 1) return false;
    song.order.pop_back();
    return true;
}

std::string to_text(const Song &song)
{
    std::string out = std::string(kHeader) + "\n";
    out += "bpm " + std::to_string(song.bpm) + "\n";
    out += "env";
    for (uint8_t e : song.envelope) out += " " + std::to_string(e);
    out += "\norder";
    for (uint8_t o : song.order) out += " " + std::to_string(o);
    out += "\n";
    for (int p = 0; p < kMaxPatterns; ++p) {
        const Pattern &pattern = song.patterns[static_cast<std::size_t>(p)];
        if (pattern_empty(pattern)) continue;
        out += "pattern " + std::to_string(p) + "\n";
        for (int r = 0; r < kRows; ++r) {
            const auto &row = pattern.rows[static_cast<std::size_t>(r)];
            bool used = false;
            for (const Cell &cell : row) used = used || cell.note != kEmpty;
            if (!used) continue;
            out += "0123456789ABCDEF"[r];
            for (int c = 0; c < kChannels; ++c)
                out += std::string(c ? " | " : " ") + cell_text(row[static_cast<std::size_t>(c)]);
            out += "\n";
        }
    }
    return out;
}

bool from_text(const std::string &text, Song &song)
{
    Song next;
    bool header = false;
    int current = -1;
    std::vector<int> values;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line[0] == '#') continue;
        if (!header) {
            if (line != kHeader) return false;
            header = true;
            continue;
        }
        if (line.compare(0, 4, "bpm ") == 0) {
            if (!parse_ints(line.substr(4), values) || values.size() != 1) return false;
            if (values[0] < kMinBpm || values[0] > kMaxBpm) return false;
            next.bpm = values[0];
        } else if (line.compare(0, 4, "env ") == 0) {
            if (!parse_ints(line.substr(4), values) || values.size() != static_cast<std::size_t>(kChannels))
                return false;
            for (int c = 0; c < kChannels; ++c) {
                if (values[static_cast<std::size_t>(c)] > kMaxEnvelope) return false;
                next.envelope[static_cast<std::size_t>(c)] = static_cast<uint8_t>(values[static_cast<std::size_t>(c)]);
            }
        } else if (line.compare(0, 6, "order ") == 0) {
            if (!parse_ints(line.substr(6), values) || values.empty() ||
                values.size() > static_cast<std::size_t>(kMaxOrder))
                return false;
            next.order.clear();
            for (int value : values) {
                if (value >= kMaxPatterns) return false;
                next.order.push_back(static_cast<uint8_t>(value));
            }
        } else if (line.compare(0, 8, "pattern ") == 0) {
            if (!parse_ints(line.substr(8), values) || values.size() != 1 || values[0] >= kMaxPatterns) return false;
            current = values[0];
        } else {
            if (current < 0 || line.size() < 2 || line[1] != ' ') return false;
            if (!parse_row(line, next.patterns[static_cast<std::size_t>(current)])) return false;
        }
    }
    if (!header) return false;
    song = next;
    return true;
}

Song demo_song()
{
    Song song;
    from_text(kDemo, song);
    return song;
}

Synth::Synth() = default;

void Synth::note_on(int channel, int note, int inst, int vol, int envelope, int gate_samples)
{
    if (channel < 0 || channel >= kChannels || note < kMinNote || note > kMaxNote) return;
    Voice &v = voices_[static_cast<std::size_t>(channel)];
    v = Voice{};
    v.inst = std::clamp(inst, 0, kInstrumentCount[static_cast<std::size_t>(channel)] - 1);
    v.vol = std::clamp(vol, 0, kMaxVolume);
    v.on = v.vol > 0;
    const double hz = note_frequency(note);
    v.step = phase_step(hz);
    // The LFSR is clocked at eight times the note frequency, at most four shifts per sample.
    v.noise_step = static_cast<uint64_t>(std::min(hz * 8.0, kRate * 4.0) * 4294967296.0 / kRate);
    v.env_period = std::clamp(envelope, 0, kMaxEnvelope) * kRate / 64;
    v.gate = std::max(gate_samples, 0);
}

void Synth::note_off(int channel)
{
    if (channel < 0 || channel >= kChannels) return;
    voices_[static_cast<std::size_t>(channel)].on = false;
}

void Synth::all_off()
{
    for (Voice &v : voices_) v.on = false;
}

bool Synth::silent() const
{
    for (const Voice &v : voices_)
        if (v.on) return false;
    return true;
}

void Synth::render(int16_t *out, int frames)
{
    if (frames <= 0) return;
    static constexpr uint32_t kDuty[4] = {0x20000000u, 0x40000000u, 0x80000000u, 0xC0000000u};
    mix_.assign(static_cast<std::size_t>(frames), 0);
    for (int channel = 0; channel < kChannels; ++channel) {
        Voice &v = voices_[static_cast<std::size_t>(channel)];
        int peak = 0;
        const std::array<uint8_t, 32> &wave = wave_table(channel == kWave ? v.inst : 0);
        for (int i = 0; i < frames && v.on; ++i) {
            const int amp = v.vol * kChannelPeak / kMaxVolume;
            int sample = 0;
            if (channel == kNoise) {
                v.noise_phase += v.noise_step;
                for (uint64_t shifts = v.noise_phase >> 32; shifts > 0; --shifts) {
                    const uint16_t bit = static_cast<uint16_t>((v.lfsr ^ (v.lfsr >> 1)) & 1u);
                    v.lfsr = static_cast<uint16_t>((v.lfsr >> 1) | (bit << 14));
                    if (v.inst == 1) v.lfsr = static_cast<uint16_t>((v.lfsr & ~0x40u) | (bit << 6));
                }
                v.noise_phase &= 0xFFFFFFFFu;
                sample = (v.lfsr & 1u) ? -amp : amp;
            } else if (channel == kWave) {
                sample = (2 * wave[v.phase >> 27] - 15) * amp / 15;
            } else {
                sample = v.phase < kDuty[v.inst] ? amp : -amp;
            }
            v.phase += v.step;
            mix_[static_cast<std::size_t>(i)] += sample;
            peak = std::max(peak, std::abs(sample));
            if (v.env_period > 0 && ++v.env_count >= v.env_period) {
                v.env_count = 0;
                if (--v.vol <= 0) v.on = false;
            }
            if (v.gate > 0 && --v.gate == 0) v.on = false;
        }
        level_[static_cast<std::size_t>(channel)] = static_cast<float>(peak) / kChannelPeak;
    }
    for (int i = 0; i < frames; ++i)
        out[i] = static_cast<int16_t>(std::clamp(mix_[static_cast<std::size_t>(i)], -32768, 32767));
}

void Player::play(const Song &song, int pattern)
{
    synth_.all_off();
    pattern_ = std::clamp(pattern, 0, kMaxPatterns - 1);
    order_pos_ = -1;
    for (std::size_t i = 0; i < song.order.size(); ++i) {
        if (song.order[i] == pattern_) {
            order_pos_ = static_cast<int>(i);
            break;
        }
    }
    row_ = 0;
    remaining_ = 0;
    carry_ = 0;
    loop_ = true;
    fresh_ = true;
    playing_ = true;
}

void Player::play_song(const Song &song, bool loop)
{
    play(song, song.order.empty() ? 0 : song.order[0]);
    loop_ = loop;
}

void Player::stop()
{
    playing_ = false;
    synth_.all_off();
}

void Player::preview(int channel, const Cell &cell, int envelope)
{
    if (cell.note == kOff)
        synth_.note_off(channel);
    else if (cell.note != kEmpty)
        synth_.note_on(channel, cell.note, cell.inst, cell.vol, envelope, kPreviewSamples);
}

void Player::begin_row(const Song &song)
{
    const auto &row = song.patterns[static_cast<std::size_t>(pattern_)].rows[static_cast<std::size_t>(row_)];
    for (int channel = 0; channel < kChannels; ++channel) {
        const Cell &cell = row[static_cast<std::size_t>(channel)];
        if (cell.note == kOff)
            synth_.note_off(channel);
        else if (cell.note != kEmpty)
            synth_.note_on(channel, cell.note, cell.inst, cell.vol, song.envelope[static_cast<std::size_t>(channel)]);
    }
    // A row is rate * 60 / (bpm * rows per beat) samples; the remainder of the
    // division carries into the next row so the tempo does not drift.
    const int bpm = clamp_bpm(song.bpm) * kRowsPerBeat;
    const int total = kRate * 60 + carry_;
    remaining_ = total / bpm;
    carry_ = total % bpm;
    fresh_ = false;
}

void Player::advance(const Song &song)
{
    fresh_ = true;
    if (++row_ < kRows) return;
    row_ = 0;
    if (order_pos_ < 0 || song.order.empty()) {
        if (!loop_) playing_ = false;
        return;
    }
    if (++order_pos_ >= static_cast<int>(song.order.size())) {
        order_pos_ = 0;
        if (!loop_) {
            playing_ = false;
            return;
        }
    }
    pattern_ = std::min<int>(song.order[static_cast<std::size_t>(order_pos_)], kMaxPatterns - 1);
}

void Player::render(const Song &song, int16_t *out, int frames)
{
    while (frames > 0) {
        int count = frames;
        if (playing_) {
            if (fresh_) begin_row(song);
            count = std::min(count, remaining_);
        }
        synth_.render(out, count);
        out += count;
        frames -= count;
        if (playing_) {
            remaining_ -= count;
            if (remaining_ <= 0) advance(song);
        }
    }
}

std::vector<int16_t> render_song(const Song &song)
{
    std::vector<int16_t> out;
    Player player;
    player.play_song(song, false);
    int16_t block[1024];
    // The carried remainder makes the row boundaries the floor of the exact
    // position, so the whole song is this many samples.
    const int64_t rows = static_cast<int64_t>(song.order.empty() ? 1 : song.order.size()) * kRows;
    const int64_t total = rows * kRate * 60 / (clamp_bpm(song.bpm) * kRowsPerBeat);
    out.reserve(static_cast<std::size_t>(total) + kTailSamples);
    for (int64_t done = 0; done < total;) {
        const int count = static_cast<int>(std::min<int64_t>(total - done, 1024));
        player.render(song, block, count);
        out.insert(out.end(), block, block + count);
        done += count;
    }
    const std::size_t body = out.size();
    out.resize(body + kTailSamples);
    for (int done = 0; done < kTailSamples;) {
        const int count = std::min<int>(kTailSamples - done, 1024);
        player.render(song, block, count);
        for (int i = 0; i < count; ++i)
            out[body + static_cast<std::size_t>(done + i)] =
                static_cast<int16_t>(block[i] * (kTailSamples - done - i) / kTailSamples);
        done += count;
    }
    return out;
}

std::vector<uint8_t> wav_bytes(const std::vector<int16_t> &samples, int rate)
{
    const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * 2);
    std::vector<uint8_t> out;
    out.reserve(44 + data_bytes);
    auto tag = [&out](const char *text) { out.insert(out.end(), text, text + 4); };
    tag("RIFF");
    put_le(out, 36 + data_bytes, 4);
    tag("WAVE");
    tag("fmt ");
    put_le(out, 16, 4);
    put_le(out, 1, 2);
    put_le(out, 1, 2);
    put_le(out, static_cast<uint32_t>(rate), 4);
    put_le(out, static_cast<uint32_t>(rate) * 2, 4);
    put_le(out, 2, 2);
    put_le(out, 16, 2);
    tag("data");
    put_le(out, data_bytes, 4);
    for (int16_t s : samples) put_le(out, static_cast<uint16_t>(s), 2);
    return out;
}

} // namespace tracker

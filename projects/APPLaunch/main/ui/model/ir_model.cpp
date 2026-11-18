/*
 * SPDX-License-Identifier: MIT
 */

#include "ir_model.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ir {

namespace {

constexpr int32_t kNecLeaderPulse = 9000;
constexpr int32_t kNecLeaderSpace = 4500;
constexpr int32_t kNecRepeatSpace = 2250;
constexpr int32_t kNecBitPulse = 560;
constexpr int32_t kNecZeroSpace = 560;
constexpr int32_t kNecOneSpace = 1690;
constexpr int kNecBits = 32;

bool near(int32_t value, int32_t target)
{
    return value >= target - target / 4 && value <= target + target / 4;
}

void append(Durations &out, int32_t value)
{
    if (value == 0) return;
    if (!out.empty() && (out.back() > 0) == (value > 0)) {
        out.back() += value;
        return;
    }
    out.push_back(value);
}

std::vector<std::string> split_words(const std::string &line)
{
    std::vector<std::string> words;
    std::size_t at = 0;
    while (at < line.size()) {
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t' || line[at] == '\r')) ++at;
        std::size_t end = at;
        while (end < line.size() && line[end] != ' ' && line[end] != '\t' && line[end] != '\r') ++end;
        if (end > at) words.push_back(line.substr(at, end - at));
        at = end;
    }
    return words;
}

bool parse_number(const std::string &word, int base, long long &value)
{
    if (word.empty()) return false;
    char *end = nullptr;
    value = std::strtoll(word.c_str(), &end, base);
    return end && *end == '\0';
}

bool known_protocol(const std::string &name)
{
    return name == "nec" || name == "necx" || name == "nec32" || name == "raw";
}

} // namespace

bool FrameAssembler::finish(Durations &frame)
{
    if (current_.empty()) return false;
    if (current_.back() < 0) current_.pop_back();
    frame.swap(current_);
    current_.clear();
    return !frame.empty();
}

bool FrameAssembler::feed(uint32_t sample, Durations &frame)
{
    const uint32_t type = sample & kMode2TypeMask;
    const int32_t value = static_cast<int32_t>(sample & kMode2ValueMask);
    if (type == kMode2Pulse) {
        append(current_, value);
        return false;
    }
    if (type == kMode2Space) {
        if (current_.empty()) return false;
        if (value >= kFrameGapUs) return finish(frame);
        append(current_, -value);
        return false;
    }
    if (type == kMode2Timeout) return finish(frame);
    if (type == kMode2Frequency) {
        carrier_ = sample & kMode2ValueMask;
        return false;
    }
    if (type == kMode2Overflow) {
        // linux/lirc.h only names this sample type. It is taken to mean that
        // samples were lost, so the partial frame is dropped (unverified).
        current_.clear();
    }
    return false;
}

bool FrameAssembler::flush(Durations &frame)
{
    return finish(frame);
}

std::vector<Durations> split_frames(const Durations &durations, int32_t gap_us)
{
    std::vector<Durations> frames;
    Durations current;
    auto close = [&] {
        if (!current.empty() && current.back() < 0) current.pop_back();
        if (!current.empty()) frames.push_back(current);
        current.clear();
    };
    for (int32_t value : durations) {
        if (value > 0) {
            append(current, value);
        } else if (value < 0) {
            if (current.empty()) continue;
            if (-value >= gap_us)
                close();
            else
                append(current, value);
        }
    }
    close();
    return frames;
}

Durations encode_nec(const Code &code)
{
    uint8_t bytes[4];
    if (code.protocol == "nec32") {
        bytes[0] = static_cast<uint8_t>(code.address);
        bytes[1] = static_cast<uint8_t>(code.address >> 8);
        bytes[2] = static_cast<uint8_t>(code.command);
        bytes[3] = static_cast<uint8_t>(code.command >> 8);
    } else {
        bytes[0] = static_cast<uint8_t>(code.address);
        bytes[1] = code.protocol == "necx" ? static_cast<uint8_t>(code.address >> 8)
                                           : static_cast<uint8_t>(~code.address);
        bytes[2] = static_cast<uint8_t>(code.command);
        bytes[3] = static_cast<uint8_t>(~code.command);
    }
    Durations out;
    out.reserve(2 + 2 * kNecBits + 1);
    out.push_back(kNecLeaderPulse);
    out.push_back(-kNecLeaderSpace);
    for (int bit = 0; bit < kNecBits; ++bit) {
        const bool one = (bytes[bit / 8] >> (bit % 8)) & 1;
        out.push_back(kNecBitPulse);
        out.push_back(one ? -kNecOneSpace : -kNecZeroSpace);
    }
    out.push_back(kNecBitPulse);
    return out;
}

Durations encode_nec_repeat()
{
    return {kNecLeaderPulse, -kNecRepeatSpace, kNecBitPulse};
}

bool decode_nec(const Durations &frame, Code &code)
{
    if (frame.size() < 3 || !near(frame[0], kNecLeaderPulse) || frame[1] >= 0) return false;
    const int32_t leader_space = -frame[1];
    // Receiver modules stretch pulses and shrink spaces, so the short marks
    // get a wide window.
    auto is_mark = [](int32_t value) { return value >= 250 && value <= 1000; };
    if (near(leader_space, kNecRepeatSpace)) {
        if (frame.size() != 3 || !is_mark(frame[2])) return false;
        code = Code{};
        code.protocol = "nec";
        code.repeat = true;
        return true;
    }
    if (!near(leader_space, kNecLeaderSpace)) return false;
    if (frame.size() != static_cast<std::size_t>(2 + 2 * kNecBits + 1)) return false;
    uint8_t bytes[4] = {0, 0, 0, 0};
    for (int bit = 0; bit < kNecBits; ++bit) {
        const int32_t mark = frame[2 + 2 * bit];
        const int32_t space = -frame[3 + 2 * bit];
        if (!is_mark(mark) || space < 200 || space > 2400) return false;
        if (space > (kNecZeroSpace + kNecOneSpace) / 2) bytes[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
    }
    if (!is_mark(frame.back())) return false;
    code = Code{};
    const bool command_ok = static_cast<uint8_t>(bytes[2] ^ bytes[3]) == 0xFF;
    const bool address_ok = static_cast<uint8_t>(bytes[0] ^ bytes[1]) == 0xFF;
    if (!command_ok) {
        code.protocol = "nec32";
        code.address = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8);
        code.command = static_cast<uint32_t>(bytes[2]) | (static_cast<uint32_t>(bytes[3]) << 8);
    } else if (!address_ok) {
        code.protocol = "necx";
        code.address = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8);
        code.command = bytes[2];
    } else {
        code.protocol = "nec";
        code.address = bytes[0];
        code.command = bytes[2];
    }
    return true;
}

uint32_t raw_hash(const Durations &frame)
{
    // FNV-1a over the frame length and one symbol per duration.
    uint32_t hash = 2166136261u;
    auto mix = [&hash](uint32_t value) {
        hash ^= value;
        hash *= 16777619u;
    };
    mix(static_cast<uint32_t>(frame.size()));
    for (std::size_t i = 2; i < frame.size(); ++i) {
        const int64_t before = std::llabs(static_cast<long long>(frame[i - 2]));
        const int64_t now = std::llabs(static_cast<long long>(frame[i]));
        // Same when within 3:4 of each other; protocol symbols differ by 1:2 or more.
        uint32_t symbol = 1;
        if (now * 4 < before * 3)
            symbol = 0;
        else if (before * 4 < now * 3)
            symbol = 2;
        mix(symbol);
    }
    return hash;
}

Code decode(const Durations &frame)
{
    Code code;
    if (decode_nec(frame, code)) return code;
    if (frame.size() < kMinFrameDurations) return Code{};
    code.protocol = "raw";
    code.command = raw_hash(frame);
    return code;
}

int slot_index(const std::string &name)
{
    for (std::size_t i = 0; i < kSlotCount; ++i)
        if (name == kSlotNames[i]) return static_cast<int>(i);
    return -1;
}

std::string serialize_remote(const Remote &remote)
{
    std::string out = "# slot protocol address command carrier durations(us, +pulse -space)\n";
    char field[48];
    for (std::size_t i = 0; i < kSlotCount; ++i) {
        const Button &button = remote.buttons[i];
        if (!button.learned()) continue;
        std::snprintf(field, sizeof(field), " %X %X %u", static_cast<unsigned>(button.code.address),
                      static_cast<unsigned>(button.code.command), static_cast<unsigned>(button.carrier_hz));
        out += kSlotNames[i];
        out += ' ';
        out += button.code.protocol;
        out += field;
        for (int32_t value : button.raw) {
            std::snprintf(field, sizeof(field), " %d", static_cast<int>(value));
            out += field;
        }
        out += '\n';
    }
    return out;
}

int parse_remote(const std::string &text, Remote &remote)
{
    remote = Remote{};
    int count = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        const std::vector<std::string> words = split_words(line);
        if (words.size() < 5 || words[0][0] == '#') continue;
        const int slot = slot_index(words[0]);
        long long address = 0, command = 0, carrier = 0;
        if (slot < 0 || !known_protocol(words[1]) || !parse_number(words[2], 16, address) ||
            !parse_number(words[3], 16, command) || !parse_number(words[4], 10, carrier))
            continue;
        if (address < 0 || address > 0xFFFFFFFFll || command < 0 || command > 0xFFFFFFFFll || carrier < 0 ||
            carrier > 1000000)
            continue;
        Button button;
        button.code.protocol = words[1];
        button.code.address = static_cast<uint32_t>(address);
        button.code.command = static_cast<uint32_t>(command);
        button.carrier_hz = carrier > 0 ? static_cast<uint32_t>(carrier) : kDefaultCarrierHz;
        bool ok = true;
        for (std::size_t i = 5; i < words.size() && ok; ++i) {
            long long value = 0;
            ok = parse_number(words[i], 10, value) && value != 0 && value > -0x1000000 && value < 0x1000000;
            if (ok) button.raw.push_back(static_cast<int32_t>(value));
        }
        if (!ok || (button.code.protocol == "raw" && button.raw.empty())) continue;
        if (!remote.buttons[static_cast<std::size_t>(slot)].learned()) ++count;
        remote.buttons[static_cast<std::size_t>(slot)] = button;
    }
    return count;
}

std::string remote_path(const std::string &home, int number)
{
    return home + "/ir/remote-" + std::to_string(number) + ".txt";
}

Durations button_durations(const Button &button)
{
    if (!button.learned()) return {};
    const std::string &protocol = button.code.protocol;
    if (protocol == "nec" || protocol == "necx" || protocol == "nec32") return encode_nec(button.code);
    return button.raw;
}

std::vector<uint32_t> tx_buffer(const Durations &durations)
{
    Durations merged;
    for (int32_t value : durations) {
        if (merged.empty() && value <= 0) continue;
        append(merged, value);
    }
    while (!merged.empty() && merged.back() < 0) merged.pop_back();
    if (merged.size() > kMaxTxDurations) merged.resize(kMaxTxDurations);
    std::vector<uint32_t> out;
    out.reserve(merged.size());
    for (int32_t value : merged) out.push_back(static_cast<uint32_t>(value < 0 ? -value : value));
    return out;
}

std::vector<uint8_t> burst_columns(const Durations &frame, int width)
{
    std::vector<uint8_t> columns(width > 0 ? static_cast<std::size_t>(width) : 0, 0);
    int64_t total = 0;
    for (int32_t value : frame) total += value < 0 ? -static_cast<int64_t>(value) : value;
    if (columns.empty() || total <= 0) return columns;
    int64_t at = 0;
    for (int32_t value : frame) {
        const int64_t length = value < 0 ? -static_cast<int64_t>(value) : value;
        if (value > 0) {
            const int64_t first = at * width / total;
            const int64_t last = std::min<int64_t>((at + length - 1) * width / total, width - 1);
            for (int64_t x = first; x <= last; ++x) columns[static_cast<std::size_t>(x)] = 1;
        }
        at += length;
    }
    return columns;
}

std::string code_label(const Code &code)
{
    if (!code.valid()) return {};
    char text[32];
    if (code.protocol == "raw")
        std::snprintf(text, sizeof(text), "%08X", static_cast<unsigned>(code.command));
    else if (code.protocol == "nec")
        std::snprintf(text, sizeof(text), "%02X:%02X", static_cast<unsigned>(code.address),
                      static_cast<unsigned>(code.command));
    else if (code.protocol == "necx")
        std::snprintf(text, sizeof(text), "%04X:%02X", static_cast<unsigned>(code.address),
                      static_cast<unsigned>(code.command));
    else
        std::snprintf(text, sizeof(text), "%04X:%04X", static_cast<unsigned>(code.address),
                      static_cast<unsigned>(code.command));
    return text;
}

std::string describe(const Code &code)
{
    if (!code.valid()) return {};
    char text[48];
    if (code.protocol == "raw")
        std::snprintf(text, sizeof(text), "raw %08X", static_cast<unsigned>(code.command));
    else if (code.protocol == "nec")
        std::snprintf(text, sizeof(text), "addr %02X  cmd %02X", static_cast<unsigned>(code.address),
                      static_cast<unsigned>(code.command));
    else if (code.protocol == "necx")
        std::snprintf(text, sizeof(text), "addr %04X  cmd %02X", static_cast<unsigned>(code.address),
                      static_cast<unsigned>(code.command));
    else
        std::snprintf(text, sizeof(text), "addr %04X  cmd %04X", static_cast<unsigned>(code.address),
                      static_cast<unsigned>(code.command));
    return text;
}

std::string carrier_label(uint32_t hz)
{
    char text[16];
    if (hz % 1000 == 0)
        std::snprintf(text, sizeof(text), "%uk", static_cast<unsigned>(hz / 1000));
    else
        std::snprintf(text, sizeof(text), "%.1fk", hz / 1000.0);
    return text;
}

} // namespace ir

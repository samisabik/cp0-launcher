/*
 * SPDX-License-Identifier: MIT
 */

#include "rec_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rec {
namespace {

uint32_t le32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t le16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

bool valid_stem(const std::string &stem)
{
    if (stem.size() != 15 || stem[8] != '_') return false;
    for (std::size_t i = 0; i < stem.size(); ++i)
        if (i != 8 && (stem[i] < '0' || stem[i] > '9')) return false;
    return true;
}

} // namespace

std::string recording_stem(int year, int month, int day, int hour, int minute, int second)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%04d%02d%02d_%02d%02d%02d", year, month, day, hour, minute, second);
    return text;
}

std::string stem_label(const std::string &stem)
{
    if (!valid_stem(stem)) return stem;
    return stem.substr(4, 2) + "-" + stem.substr(6, 2) + " " + stem.substr(9, 2) + ":" + stem.substr(11, 2);
}

std::string format_duration(uint32_t ms)
{
    const uint32_t seconds = ms / 1000;
    char text[16];
    std::snprintf(text, sizeof(text), "%u:%02u", seconds / 60, seconds % 60);
    return text;
}

uint32_t wav_duration_ms(const uint8_t *header, std::size_t header_bytes, uint64_t file_size)
{
    if (header_bytes < 12 || std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0)
        return 0;
    uint32_t byte_rate = 0;
    std::size_t offset = 12;
    while (offset + 8 <= header_bytes) {
        const uint8_t *chunk = header + offset;
        const uint32_t size = le32(chunk + 4);
        if (std::memcmp(chunk, "fmt ", 4) == 0 && offset + 8 + 16 <= header_bytes) {
            const uint16_t channels = le16(chunk + 10);
            const uint32_t rate = le32(chunk + 12);
            const uint16_t bits = le16(chunk + 22);
            byte_rate = rate * channels * (bits / 8);
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            if (byte_rate == 0) return 0;
            const uint64_t data_start = offset + 8;
            if (file_size <= data_start) return 0;
            uint64_t data = file_size - data_start;
            if (size != 0 && size != 0xFFFFFFFFu && size != 0x7FFFFFFFu) data = std::min<uint64_t>(data, size);
            return static_cast<uint32_t>(data * 1000 / byte_rate);
        }
        offset += 8 + size + (size & 1);
    }
    return 0;
}

std::string clean_transcript(const std::string &raw)
{
    std::string out;
    int depth = 0;
    bool space = false;
    for (char c : raw) {
        if (c == '[' || c == '(') {
            ++depth;
            continue;
        }
        if ((c == ']' || c == ')') && depth > 0) {
            --depth;
            space = !out.empty();
            continue;
        }
        if (depth > 0) continue;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

std::string clip_text(const std::string &text, std::size_t max_chars)
{
    std::size_t chars = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) continue;
        if (chars == max_chars) return text.substr(0, i);
        ++chars;
    }
    return text;
}

int peak_level(const uint8_t *pcm, std::size_t bytes)
{
    int peak = 0;
    for (std::size_t i = 0; i + 1 < bytes; i += 2) {
        const int sample = static_cast<int16_t>(le16(pcm + i));
        peak = std::max(peak, std::abs(sample));
    }
    if (peak == 0) return 0;
    const double dbfs = 20.0 * std::log10(peak / 32768.0);
    return std::clamp(static_cast<int>(std::lround((dbfs + 60.0) * 100.0 / 60.0)), 0, 100);
}

uint32_t transcribe_estimate_ms(uint32_t duration_ms)
{
    return 3000 + duration_ms / 4 * 5;
}

} // namespace rec

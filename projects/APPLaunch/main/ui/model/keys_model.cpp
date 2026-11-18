/*
 * SPDX-License-Identifier: MIT
 */

#include "keys_model.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace keys {

namespace {

struct Pulse {
    uint32_t start_us = 0;
    uint32_t high_us = 0;
    uint32_t low_us = 0;
};

struct Packet {
    std::size_t first = 0;
    std::size_t count = 0;
    bool clean = false;
    bool sync = false;
    std::string bits;
    uint64_t short_sum = 0;
    int short_count = 0;
};

uint32_t magnitude(int32_t d)
{
    return static_cast<uint32_t>(d < 0 ? -static_cast<int64_t>(d) : d);
}

// Same-sign neighbours merge and leading lows drop, so every pulse is one high
// followed by one low (zero for a capture that ends high).
std::vector<Pulse> to_pulses(const std::vector<int32_t> &durations, uint32_t &total_us)
{
    std::vector<Pulse> pulses;
    uint64_t t = 0;
    for (int32_t d : durations) {
        const uint32_t length = magnitude(d);
        if (length == 0) continue;
        if (d > 0) {
            if (!pulses.empty() && pulses.back().low_us == 0)
                pulses.back().high_us += length;
            else
                pulses.push_back({static_cast<uint32_t>(std::min<uint64_t>(t, UINT32_MAX)), length, 0});
        } else if (!pulses.empty()) {
            pulses.back().low_us += length;
        }
        t += length;
    }
    total_us = static_cast<uint32_t>(std::min<uint64_t>(t, UINT32_MAX));
    return pulses;
}

bool is_short(uint32_t us, int te)
{
    return us * 2 >= static_cast<uint32_t>(te) && us * 10 <= static_cast<uint32_t>(te) * 16;
}

void decode(const std::vector<Pulse> &pulses, int te, Packet &packet)
{
    packet.sync = packet.count % 4 != 0;
    const std::size_t bit_count = packet.sync ? packet.count - 1 : packet.count;
    packet.clean = bit_count >= static_cast<std::size_t>(kMinBits);
    const uint32_t longest = static_cast<uint32_t>(te) * kGapTe;
    for (std::size_t i = 0; i < bit_count; ++i) {
        const Pulse &pulse = pulses[packet.first + i];
        const bool before_gap = i + 1 == packet.count;
        if (before_gap) {
            // The low is the gap, so only the high tells the bit.
            const bool one = !is_short(pulse.high_us, te);
            if (one && (pulse.high_us > longest || pulse.high_us * 2 < static_cast<uint32_t>(te)))
                packet.clean = false;
            packet.bits += one ? '1' : '0';
            if (!one) {
                packet.short_sum += pulse.high_us;
                ++packet.short_count;
            }
            continue;
        }
        const uint32_t low = std::min(pulse.high_us, pulse.low_us);
        const uint32_t high = std::max(pulse.high_us, pulse.low_us);
        if (!is_short(low, te) || high * 2 < low * 3 || high > longest) packet.clean = false;
        packet.bits += pulse.high_us > pulse.low_us ? '1' : '0';
        packet.short_sum += low;
        ++packet.short_count;
    }
}

std::string to_hex(const std::string &bits)
{
    std::string padded((4 - bits.size() % 4) % 4, '0');
    padded += bits;
    std::string hex;
    for (std::size_t i = 0; i + 4 <= padded.size(); i += 4) {
        int value = 0;
        for (std::size_t j = 0; j < 4; ++j) value = value * 2 + (padded[i + j] == '1' ? 1 : 0);
        hex += "0123456789ABCDEF"[value];
    }
    return hex;
}

} // namespace

int estimate_te(const std::vector<int32_t> &durations)
{
    std::vector<uint32_t> values;
    values.reserve(durations.size());
    for (int32_t d : durations)
        if (d != 0) values.push_back(magnitude(d));
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const std::size_t need = std::min(values.size(), std::max<std::size_t>(4, values.size() / 8));
    for (std::size_t first = 0; first < values.size(); ++first) {
        std::size_t last = first;
        const uint64_t limit = static_cast<uint64_t>(values[first]) * 16;
        while (last + 1 < values.size() && static_cast<uint64_t>(values[last + 1]) * 10 <= limit) ++last;
        if (last - first + 1 >= need) return static_cast<int>(values[first + (last - first) / 2]);
    }
    return static_cast<int>(values[values.size() / 2]);
}

namespace {

Analysis analyze_at(const std::vector<Pulse> &pulses, uint32_t total_us, int te)
{
    Analysis result;
    result.total_us = total_us;
    if (pulses.empty() || te <= 0) return result;
    result.te_us = te;

    std::vector<Packet> packets;
    const uint32_t gap = static_cast<uint32_t>(te) * kGapTe;
    std::size_t first = 0;
    for (std::size_t i = 0; i < pulses.size(); ++i) {
        if (pulses[i].low_us < gap && i + 1 < pulses.size()) continue;
        Packet packet;
        packet.first = first;
        packet.count = i - first + 1;
        first = i + 1;
        if (packet.count < static_cast<std::size_t>(kMinBits)) continue;
        decode(pulses, te, packet);
        packets.push_back(std::move(packet));
    }
    result.packets = static_cast<int>(packets.size());
    if (packets.empty()) return result;

    // The code most clean packets agree on; longer, then earlier, breaks a tie.
    const Packet *best = nullptr;
    int best_votes = 0;
    for (const Packet &candidate : packets) {
        if (!candidate.clean) continue;
        int votes = 0;
        for (const Packet &other : packets)
            if (other.clean && other.bits == candidate.bits) ++votes;
        if (votes > best_votes || (votes == best_votes && best && candidate.bits.size() > best->bits.size())) {
            best = &candidate;
            best_votes = votes;
        }
    }

    if (best) {
        result.confident = true;
        result.repeats = best_votes;
        uint64_t sum = 0;
        int count = 0;
        for (const Packet &packet : packets) {
            if (!packet.clean || packet.bits != best->bits) continue;
            sum += packet.short_sum;
            count += packet.short_count;
        }
        const uint64_t n = static_cast<uint64_t>(count);
        if (n > 0) result.te_us = static_cast<int>((sum + n / 2) / n);
    } else {
        for (const Packet &candidate : packets)
            if (!best || candidate.count > best->count) best = &candidate;
        for (const Packet &other : packets)
            if (other.count == best->count) ++result.repeats;
    }

    result.encoding = Encoding::Pwm;
    result.sync = best->sync;
    result.bits = best->bits;
    result.bit_count = static_cast<int>(best->bits.size());
    result.hex = to_hex(best->bits);
    const Pulse &head = pulses[best->first];
    const Pulse &tail = pulses[best->first + best->count - 1];
    result.packet_start_us = head.start_us;
    result.packet_us = tail.start_us + tail.high_us - head.start_us;
    for (int i = 0; i < result.bit_count; ++i) result.bit_edges_us.push_back(pulses[best->first + i].start_us);
    if (result.bit_count > 0) {
        if (best->sync) {
            result.bit_edges_us.push_back(tail.start_us);
        } else {
            // The last bit runs into the gap; give it the average bit length.
            const uint32_t span = tail.start_us - head.start_us;
            const uint32_t average = result.bit_count > 1 ? span / static_cast<uint32_t>(result.bit_count - 1)
                                                          : tail.high_us;
            result.bit_edges_us.push_back(tail.start_us + std::max(average, tail.high_us));
        }
    }
    return result;
}

// Every well-populated cluster of durations, shortest first, as a candidate
// base element. A receiver left open after the transmission fills a capture
// with sub-100 us noise pulses that outnumber the real symbols, so the
// shortest cluster alone is not a safe choice.
std::vector<int> te_candidates(const std::vector<int32_t> &durations)
{
    std::vector<uint32_t> values;
    for (int32_t d : durations)
        if (d != 0) values.push_back(magnitude(d));
    std::sort(values.begin(), values.end());
    std::vector<int> out;
    const std::size_t need = std::max<std::size_t>(4, values.size() / 50);
    for (std::size_t first = 0; first < values.size() && out.size() < 12;) {
        std::size_t last = first;
        const uint64_t limit = static_cast<uint64_t>(values[first]) * 16;
        while (last + 1 < values.size() && static_cast<uint64_t>(values[last + 1]) * 10 <= limit) ++last;
        if (last - first + 1 >= need) out.push_back(static_cast<int>(values[first + (last - first) / 2]));
        first = last + 1;
    }
    return out;
}

} // namespace

Analysis analyze(const std::vector<int32_t> &durations)
{
    uint32_t total_us = 0;
    const std::vector<Pulse> pulses = to_pulses(durations, total_us);
    Analysis best = analyze_at(pulses, total_us, estimate_te(durations));
    auto score = [](const Analysis &a) { return a.confident ? a.repeats * a.bit_count : 0; };
    for (int te : te_candidates(durations)) {
        Analysis candidate = analyze_at(pulses, total_us, te);
        if (score(candidate) > score(best)) best = std::move(candidate);
    }
    return best;
}

const char *encoding_label(Encoding encoding)
{
    return encoding == Encoding::Pwm ? "pwm" : "raw";
}

std::vector<uint8_t> waveform_columns(const std::vector<int32_t> &durations, uint32_t start_us, uint32_t span_us,
                                      int width)
{
    std::vector<uint8_t> columns(static_cast<std::size_t>(std::max(width, 0)), 0);
    if (width <= 0 || span_us == 0) return columns;
    const int64_t start = start_us;
    const int64_t end = start + span_us;
    int64_t t = 0;
    for (int32_t d : durations) {
        const int64_t length = magnitude(d);
        if (length == 0) continue;
        const int64_t from = std::max(t, start);
        const int64_t to = std::min(t + length, end);
        t += length;
        if (to > from) {
            const int64_t c0 = (from - start) * width / span_us;
            const int64_t c1 = std::min<int64_t>((to - 1 - start) * width / span_us, width - 1);
            const uint8_t level = d > 0 ? kColumnHigh : kColumnLow;
            for (int64_t c = c0; c <= c1; ++c) columns[static_cast<std::size_t>(c)] |= level;
        }
        if (t >= end) break;
    }
    return columns;
}

uint32_t view_span_us(const Analysis &analysis, int level, int width)
{
    level = std::clamp(level, 0, kZoomLevels - 1);
    const uint32_t floor_us = static_cast<uint32_t>(std::max(width, 1));
    const uint32_t total = std::max(analysis.total_us, floor_us);
    if (analysis.bit_count <= 0 || analysis.bit_edges_us.size() < 2 || width <= 0)
        return std::max(total >> level, floor_us);
    if (level == kZoomLevels - 1) return total;
    const double bit_us = static_cast<double>(analysis.bit_edges_us.back() - analysis.bit_edges_us.front()) /
                          analysis.bit_count;
    const int pitch = std::clamp(width / (analysis.bit_count + 1), 1, 16);
    const double span = bit_us * width / pitch / static_cast<double>(1 << level);
    return std::max(static_cast<uint32_t>(span + 0.5), floor_us);
}

uint32_t clamp_view_start(int64_t start_us, uint32_t span_us, uint32_t total_us)
{
    const int64_t limit = total_us > span_us ? static_cast<int64_t>(total_us - span_us) : 0;
    return static_cast<uint32_t>(std::clamp<int64_t>(start_us, 0, limit));
}

int column_of(uint32_t time_us, uint32_t start_us, uint32_t span_us, int width)
{
    if (span_us == 0) return 0;
    const int64_t offset = static_cast<int64_t>(time_us) - static_cast<int64_t>(start_us);
    const int64_t column = offset >= 0 ? offset * width / span_us : -((-offset * width + span_us - 1) / span_us);
    return static_cast<int>(std::clamp<int64_t>(column, -100000, 100000));
}

std::string format_duration(uint32_t us)
{
    char text[24];
    if (us < 1000)
        std::snprintf(text, sizeof(text), "%u us", static_cast<unsigned>(us));
    else if (us < 1000000)
        std::snprintf(text, sizeof(text), "%.1f ms", us / 1000.0);
    else
        std::snprintf(text, sizeof(text), "%.2f s", us / 1000000.0);
    return text;
}

std::string capture_label(const std::string &stem)
{
    std::string label = stem;
    if (label.rfind("capture-", 0) == 0) label = label.substr(8);
    bool stamped = label.size() >= 15 && label[8] == '-';
    for (std::size_t i = 0; stamped && i < 15; ++i)
        if (i != 8 && !std::isdigit(static_cast<unsigned char>(label[i]))) stamped = false;
    if (!stamped) return label;
    return label.substr(4, 4) + " " + label.substr(9, 2) + ":" + label.substr(11, 2) + ":" + label.substr(13, 2);
}

} // namespace keys

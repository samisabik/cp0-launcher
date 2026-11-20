/*
 * SPDX-License-Identifier: MIT
 */

#include "pins_model.hpp"

#include <algorithm>
#include <cstdio>

namespace pins {

namespace {

std::string trimmed(const std::string &text)
{
    std::size_t first = 0, last = text.size();
    while (first < last && (text[first] == ' ' || text[first] == '\t')) ++first;
    while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t')) --last;
    return text.substr(first, last - first);
}

bool parse_int(const std::string &text, int max, int &value)
{
    if (text.empty() || text.size() > 4) return false;
    int out = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        out = out * 10 + (c - '0');
    }
    if (out > max) return false;
    value = out;
    return true;
}

std::vector<std::string> split(const std::string &text, char separator)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find(separator, start);
        parts.push_back(trimmed(text.substr(start, end == std::string::npos ? end : end - start)));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return parts;
}

std::string scaled(int64_t value, int64_t unit, const char *suffix)
{
    char text[32];
    if (value % unit == 0)
        std::snprintf(text, sizeof(text), "%lld %s", static_cast<long long>(value / unit), suffix);
    else
        std::snprintf(text, sizeof(text), "%.1f %s", static_cast<double>(value) / static_cast<double>(unit), suffix);
    return text;
}

} // namespace

std::vector<LineRef> parse_lines(const std::string &text)
{
    std::vector<LineRef> lines;
    for (const std::string &part : split(text, ',')) {
        if (lines.size() >= kMaxLines) break;
        LineRef line;
        const std::size_t colon = part.find(':');
        if (colon == std::string::npos) {
            if (!parse_int(part, kMaxOffset, line.offset)) continue;
        } else if (!parse_int(trimmed(part.substr(0, colon)), kMaxChip, line.chip) ||
                   !parse_int(trimmed(part.substr(colon + 1)), kMaxOffset, line.offset)) {
            continue;
        }
        const bool repeat = std::any_of(lines.begin(), lines.end(), [&](const LineRef &other) {
            return other.chip == line.chip && other.offset == line.offset;
        });
        if (!repeat) lines.push_back(line);
    }
    return lines;
}

bool parse_i2c_pair(const std::string &text, std::size_t line_count, int &scl, int &sda)
{
    const std::vector<std::string> parts = split(text, ',');
    int a = 0, b = 0;
    if (parts.size() != 2 || !parse_int(parts[0], static_cast<int>(kMaxLines) - 1, a) ||
        !parse_int(parts[1], static_cast<int>(kMaxLines) - 1, b))
        return false;
    if (a == b || static_cast<std::size_t>(a) >= line_count || static_cast<std::size_t>(b) >= line_count) return false;
    scl = a;
    sda = b;
    return true;
}

int64_t nice_step(int64_t ns)
{
    int64_t decade = 1;
    while (true) {
        for (int64_t factor : {1, 2, 5})
            if (decade * factor >= ns) return decade * factor;
        if (decade > INT64_MAX / 10) return decade * 5;
        decade *= 10;
    }
}

int64_t next_step(int64_t ns, bool larger)
{
    const int64_t current = nice_step(std::clamp(ns, kMinDivNs, kMaxDivNs));
    int64_t next = current;
    if (larger) {
        next = nice_step(current + 1);
    } else {
        // The step below a 1-2-5 value is at least 0.4 of it.
        next = nice_step(current * 2 / 5);
    }
    return std::clamp(next, kMinDivNs, kMaxDivNs);
}

std::string format_time(int64_t ns)
{
    if (ns >= 1000000000) return scaled(ns, 1000000000, "s");
    if (ns >= 1000000) return scaled(ns, 1000000, "ms");
    if (ns >= 1000) return scaled(ns, 1000, "us");
    return scaled(ns, 1, "ns");
}

std::string format_hz(uint32_t hz)
{
    if (hz >= 1000000) return scaled(hz, 1000000, "MHz");
    if (hz >= 1000) return scaled(hz, 1000, "kHz");
    return scaled(hz, 1, "Hz");
}

std::vector<Edge> window(const std::vector<Edge> &edges, bool initial, int64_t start_ns, int64_t end_ns,
                         bool &level_at_start)
{
    const auto by_time = [](const Edge &edge, int64_t t) { return edge.t_ns < t; };
    const auto first = std::lower_bound(edges.begin(), edges.end(), start_ns, by_time);
    const auto last = std::lower_bound(first, edges.end(), end_ns, by_time);
    level_at_start = first == edges.begin() ? initial : (first - 1)->level;
    return std::vector<Edge>(first, last);
}

void trim(std::vector<Edge> &edges, bool &initial, std::size_t max)
{
    if (edges.size() <= max) return;
    const std::size_t drop = edges.size() - max / 2;
    initial = edges[drop - 1].level;
    edges.erase(edges.begin(), edges.begin() + static_cast<std::ptrdiff_t>(drop));
}

std::vector<uint8_t> trace_columns(const std::vector<Edge> &edges, bool initial, int64_t start_ns, int64_t span_ns,
                                   int columns)
{
    std::vector<uint8_t> out(columns > 0 ? static_cast<std::size_t>(columns) : 0, 0);
    if (out.empty() || span_ns <= 0) return out;
    bool level = initial;
    std::size_t at = 0;
    while (at < edges.size() && edges[at].t_ns < start_ns) level = edges[at++].level;
    for (int column = 0; column < columns; ++column) {
        const int64_t end = start_ns + span_ns * (column + 1) / columns;
        bool changed = false;
        while (at < edges.size() && edges[at].t_ns < end) {
            changed = changed || edges[at].level != level;
            level = edges[at++].level;
        }
        out[static_cast<std::size_t>(column)] =
            static_cast<uint8_t>((level ? kColumnHigh : 0) | (changed ? kColumnEdge : 0));
    }
    return out;
}

std::vector<I2cToken> decode_i2c(const std::vector<Edge> &scl, const std::vector<Edge> &sda, bool scl_initial,
                                 bool sda_initial)
{
    std::vector<I2cToken> tokens;
    bool scl_level = scl_initial, sda_level = sda_initial;
    bool active = false, first_byte = false;
    int bits = 0;
    unsigned byte = 0;
    std::size_t c = 0, d = 0;
    auto emit = [&tokens](I2cToken::Kind kind, int64_t t, uint8_t value = 0, bool read = false) {
        I2cToken token;
        token.kind = kind;
        token.value = value;
        token.read = read;
        token.t_ns = t;
        tokens.push_back(token);
    };
    while (c < scl.size() || d < sda.size()) {
        // With equal timestamps the clock edge goes first: data is meant to
        // change just after SCL falls.
        const bool clock = d >= sda.size() || (c < scl.size() && scl[c].t_ns <= sda[d].t_ns);
        if (clock) {
            const Edge edge = scl[c++];
            if (edge.level == scl_level) continue;
            scl_level = edge.level;
            if (!scl_level || !active) continue;
            if (bits < 8) {
                byte = (byte << 1) | (sda_level ? 1u : 0u);
                if (++bits < 8) continue;
                if (first_byte)
                    emit(I2cToken::Kind::Address, edge.t_ns, static_cast<uint8_t>(byte >> 1), (byte & 1u) != 0);
                else
                    emit(I2cToken::Kind::Data, edge.t_ns, static_cast<uint8_t>(byte));
            } else {
                emit(sda_level ? I2cToken::Kind::Nak : I2cToken::Kind::Ack, edge.t_ns);
                bits = 0;
                byte = 0;
                first_byte = false;
            }
            continue;
        }
        const Edge edge = sda[d++];
        if (edge.level == sda_level) continue;
        sda_level = edge.level;
        if (!scl_level) continue;
        if (!sda_level) {
            emit(active ? I2cToken::Kind::Restart : I2cToken::Kind::Start, edge.t_ns);
            active = true;
            first_byte = true;
        } else if (active) {
            emit(I2cToken::Kind::Stop, edge.t_ns);
            active = false;
        }
        bits = 0;
        byte = 0;
    }
    return tokens;
}

std::string format_i2c(const std::vector<I2cToken> &tokens)
{
    std::string out;
    char text[8];
    for (const I2cToken &token : tokens) {
        if (!out.empty()) out += ' ';
        switch (token.kind) {
        case I2cToken::Kind::Start:
            out += "S";
            break;
        case I2cToken::Kind::Restart:
            out += "Sr";
            break;
        case I2cToken::Kind::Address:
            std::snprintf(text, sizeof(text), "%02X%c", static_cast<unsigned>(token.value), token.read ? 'r' : 'w');
            out += text;
            break;
        case I2cToken::Kind::Data:
            std::snprintf(text, sizeof(text), "%02X", static_cast<unsigned>(token.value));
            out += text;
            break;
        case I2cToken::Kind::Ack:
            out += "a";
            break;
        case I2cToken::Kind::Nak:
            out += "n";
            break;
        case I2cToken::Kind::Stop:
            out += "P";
            break;
        }
    }
    return out;
}

uint32_t i2c_clock_hz(const std::vector<Edge> &scl)
{
    std::vector<int64_t> periods;
    int64_t previous = -1;
    for (const Edge &edge : scl) {
        if (!edge.level) continue;
        if (previous >= 0 && edge.t_ns > previous) periods.push_back(edge.t_ns - previous);
        previous = edge.t_ns;
    }
    if (periods.size() < 2) return 0;
    std::nth_element(periods.begin(), periods.begin() + static_cast<std::ptrdiff_t>(periods.size() / 2), periods.end());
    const int64_t median = periods[periods.size() / 2];
    return static_cast<uint32_t>((1000000000 + median / 2) / median);
}

int grid_address(int row, int column)
{
    if (row < 0 || row >= kGridRows || column < 0 || column >= kGridColumns) return -1;
    const int address = row * kGridColumns + column;
    return address >= kI2cFirst && address <= kI2cLast ? address : -1;
}

int count(const ProbeMap &map, Probe state)
{
    return static_cast<int>(std::count(map.begin() + kI2cFirst, map.begin() + kI2cLast + 1, state));
}

std::string found_list(const ProbeMap &map)
{
    std::string out;
    char text[8];
    for (int address = kI2cFirst; address <= kI2cLast; ++address) {
        if (map[static_cast<std::size_t>(address)] != Probe::Found) continue;
        std::snprintf(text, sizeof(text), "%s%02X", out.empty() ? "" : " ", static_cast<unsigned>(address));
        out += text;
    }
    return out;
}

} // namespace pins

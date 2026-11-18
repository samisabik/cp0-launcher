// SPDX-License-Identifier: MIT

#include "../main/ui/model/keys_model.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace keys;

namespace {

struct Jitter {
    uint32_t state = 12345;
    int percent = 8;
    // Deterministic value within +-percent of the nominal duration.
    int32_t operator()(int nominal)
    {
        state = state * 1664525u + 1013904223u;
        const int span = nominal * percent / 100;
        if (span == 0) return nominal;
        return nominal - span + static_cast<int>((state >> 8) % static_cast<uint32_t>(2 * span + 1));
    }
};

std::string hex_to_bits(const std::string &hex)
{
    std::string bits;
    for (char c : hex) {
        const int value = c <= '9' ? c - '0' : c - 'A' + 10;
        for (int i = 3; i >= 0; --i) bits += (value >> i) & 1 ? '1' : '0';
    }
    return bits;
}

// One PWM packet: 0 is te high + ratio*te low, 1 is the reverse. With sync, a
// te pulse follows the data before the gap.
void append_packet(std::vector<int32_t> &out, const std::string &bits, int te, int ratio, bool sync, int gap_us,
                   Jitter &jitter)
{
    for (std::size_t i = 0; i < bits.size(); ++i) {
        const bool one = bits[i] == '1';
        const bool last = i + 1 == bits.size();
        out.push_back(jitter(one ? ratio * te : te));
        out.push_back(!sync && last ? -gap_us : -jitter(one ? te : ratio * te));
    }
    if (sync) {
        out.push_back(jitter(te));
        out.push_back(-gap_us);
    }
}

std::vector<int32_t> capture(const std::string &bits, int te, int ratio, bool sync, int repeats, Jitter &jitter)
{
    std::vector<int32_t> out;
    for (int i = 0; i < repeats; ++i) append_packet(out, bits, te, ratio, sync, 31 * te, jitter);
    return out;
}

void test_empty()
{
    const Analysis a = analyze({});
    assert(a.encoding == Encoding::Unknown);
    assert(!a.confident);
    assert(a.bit_count == 0 && a.te_us == 0 && a.repeats == 0 && a.packets == 0);
    assert(a.bits.empty() && a.hex.empty() && a.bit_edges_us.empty());
    assert(a.total_us == 0);
    assert(estimate_te({}) == 0);

    // Only lows, or a couple of pulses, is not a packet either.
    assert(analyze({-500, -900}).bit_count == 0);
    const Analysis few = analyze({400, -1200, 400, -1200, 1200, -400});
    assert(few.bit_count == 0 && !few.confident && few.packets == 0);
    assert(few.total_us == 4800);
}

void test_pwm_with_sync()
{
    Jitter jitter;
    const std::string bits = hex_to_bits("A53C91");
    const std::vector<int32_t> durations = capture(bits, 350, 3, true, 5, jitter);
    const Analysis a = analyze(durations);
    assert(a.encoding == Encoding::Pwm);
    assert(a.confident);
    assert(a.sync);
    assert(a.bit_count == 24);
    assert(a.bits == bits);
    assert(a.hex == "A53C91");
    assert(a.repeats == 5 && a.packets == 5);
    assert(std::abs(a.te_us - 350) <= 20);
    assert(std::abs(estimate_te(durations) - 350) <= 35);

    // Edges: one per bit plus the end, rising, about four elements apart.
    assert(a.bit_edges_us.size() == 25);
    assert(a.packet_start_us == 0 && a.bit_edges_us.front() == 0);
    for (std::size_t i = 1; i < a.bit_edges_us.size(); ++i) {
        const int length = static_cast<int>(a.bit_edges_us[i] - a.bit_edges_us[i - 1]);
        assert(std::abs(length - 4 * 350) <= 4 * 350 * 8 / 100 + 1);
    }
    // The packet ends after the sync pulse and before the gap.
    assert(a.packet_us > a.bit_edges_us.back() && a.packet_us < a.bit_edges_us.back() + 2 * 350);
    uint64_t total = 0;
    for (int32_t d : durations) total += static_cast<uint64_t>(std::abs(d));
    assert(a.total_us == total);
}

void test_pwm_without_sync()
{
    Jitter jitter;
    jitter.state = 99;
    const std::string bits = hex_to_bits("0F5AC3E1");
    const Analysis a = analyze(capture(bits, 200, 2, false, 3, jitter));
    assert(a.confident && !a.sync);
    assert(a.bit_count == 32);
    assert(a.bits == bits);
    assert(a.hex == "0F5AC3E1");
    assert(a.repeats == 3);
    assert(std::abs(a.te_us - 200) <= 12);
    assert(a.bit_edges_us.size() == 33);
}

void test_odd_bit_count()
{
    Jitter jitter;
    jitter.state = 7;
    // 25 bits pad to seven nibbles: 1 0101 0101 1100 1100 1111 0000.
    const std::string bits = "1010101011100110011110000";
    const Analysis a = analyze(capture(bits, 500, 3, true, 4, jitter));
    assert(a.confident && a.sync);
    assert(a.bit_count == 25);
    assert(a.bits == bits);
    assert(a.hex == "155CCF0");
    assert(a.repeats == 4);
    assert(std::abs(a.te_us - 500) <= 30);
}

void test_noise()
{
    Jitter jitter;
    jitter.state = 4242;
    const std::string bits = hex_to_bits("C0FFEE");
    std::vector<int32_t> durations;
    // Receiver glitches ahead of the transmission.
    for (int i = 0; i < 9; ++i) {
        durations.push_back(25 + 13 * i);
        durations.push_back(-(900 + 7919 * i % 6000));
    }
    for (int i = 0; i < 5; ++i) {
        const std::size_t at = durations.size();
        append_packet(durations, bits, 400, 3, true, 12400, jitter);
        // A glitch splits the long low of the third packet's fifth bit.
        if (i == 2) {
            const int32_t low = durations[at + 9];
            durations[at + 9] = low / 2;
            durations.insert(durations.begin() + static_cast<long>(at) + 10, {45, low / 2 + 45});
        }
        // A glitch in the middle of a gap.
        if (i == 3) {
            durations.back() = -6000;
            durations.insert(durations.end(), {60, -6340});
        }
    }
    const Analysis a = analyze(durations);
    assert(a.confident);
    assert(a.bits == bits);
    assert(a.hex == "C0FFEE");
    assert(a.bit_count == 24);
    assert(a.packets == 5);
    assert(a.repeats == 4);
    assert(std::abs(a.te_us - 400) <= 25);
    // The decoded packet starts after the leading glitches.
    assert(a.packet_start_us > 9 * 900);
}

void test_not_pwm()
{
    // Equal highs and lows carry no PWM bit: reported, but not trusted.
    std::vector<int32_t> durations;
    for (int repeat = 0; repeat < 2; ++repeat) {
        for (int i = 0; i < 16; ++i) {
            durations.push_back(300);
            durations.push_back(i == 15 ? -9000 : -300);
        }
    }
    const Analysis a = analyze(durations);
    assert(!a.confident);
    assert(a.bit_count == 16);
    assert(a.packets == 2 && a.repeats == 2);
    assert(a.te_us == 300);
    assert(a.hex.size() == 4);
}

void test_waveform()
{
    const std::vector<int32_t> durations = {100, -300, 200, -400};
    // One column per 100 us.
    std::vector<uint8_t> cols = waveform_columns(durations, 0, 1000, 10);
    const std::vector<uint8_t> expected = {kColumnHigh, kColumnLow, kColumnLow, kColumnLow, kColumnHigh,
                                           kColumnHigh, kColumnLow, kColumnLow, kColumnLow, kColumnLow};
    assert(cols == expected);

    // A wider column holds both levels where an edge falls inside it.
    cols = waveform_columns(durations, 0, 1000, 4);
    assert(cols.size() == 4);
    assert(cols[0] == (kColumnHigh | kColumnLow));
    assert(cols[1] == (kColumnHigh | kColumnLow));
    assert(cols[2] == (kColumnHigh | kColumnLow));
    assert(cols[3] == kColumnLow);

    // A window scrolled past the end is empty there.
    cols = waveform_columns(durations, 800, 400, 4);
    assert(cols[0] == kColumnLow && cols[1] == kColumnLow && cols[2] == 0 && cols[3] == 0);

    // Zoomed into the second pulse.
    cols = waveform_columns(durations, 400, 200, 8);
    for (uint8_t c : cols) assert(c == kColumnHigh);

    assert(waveform_columns(durations, 0, 0, 8) == std::vector<uint8_t>(8, 0));
    assert(waveform_columns(durations, 0, 100, 0).empty());
    assert(waveform_columns({}, 0, 100, 3) == std::vector<uint8_t>(3, 0));
}

void test_view()
{
    Jitter jitter;
    jitter.percent = 0;
    const Analysis a = analyze(capture(hex_to_bits("A53C91"), 350, 3, true, 5, jitter));
    // 24 bits and the sync pulse at 12 px per bit in 300 px; one bit is 1400 us.
    const uint32_t span = view_span_us(a, 0, 300);
    assert(span == 1400u * 300u / 12u);
    assert(view_span_us(a, 1, 300) == span / 2);
    assert(view_span_us(a, 2, 300) == span / 4);
    assert(view_span_us(a, kZoomLevels - 1, 300) == a.total_us);
    assert(view_span_us(a, 99, 300) == a.total_us);
    for (int bit = 0; bit <= 24; ++bit) assert(column_of(a.bit_edges_us[bit], 0, span, 300) == 12 * bit);
    assert(column_of(0, 1400, span, 300) == -12);

    // No code: the whole capture, then halves.
    Analysis raw;
    raw.total_us = 80000;
    assert(view_span_us(raw, 0, 300) == 80000);
    assert(view_span_us(raw, 1, 300) == 40000);
    assert(view_span_us(raw, 3, 300) == 10000);
    assert(view_span_us(Analysis{}, 0, 300) == 300);

    assert(clamp_view_start(-5, 1000, 5000) == 0);
    assert(clamp_view_start(1200, 1000, 5000) == 1200);
    assert(clamp_view_start(9000, 1000, 5000) == 4000);
    assert(clamp_view_start(100, 6000, 5000) == 0);
}

void test_text()
{
    assert(format_duration(350) == "350 us");
    assert(format_duration(12400) == "12.4 ms");
    assert(format_duration(1200000) == "1.20 s");
    assert(capture_label("capture-20261115-215411-433.920") == "1115 21:54:11");
    assert(capture_label("gate-remote") == "gate-remote");
    assert(capture_label("capture-garage") == "garage");
    assert(capture_label("") == "");
    assert(std::string(encoding_label(Encoding::Pwm)) == "pwm");
    assert(std::string(encoding_label(Encoding::Unknown)) == "raw");
}

// A receiver left open after the transmission: three clean packets followed
// by far more sub-200 us noise pulses than there are real symbols.
void test_noise_tail()
{
    Jitter jitter;
    const std::string bits = hex_to_bits("A9C1D593");
    std::vector<int32_t> durations = capture(bits, 500, 3, true, 3, jitter);
    Jitter noise;
    noise.percent = 70;
    for (int i = 0; i < 1500; ++i) {
        durations.push_back(noise(110));
        durations.push_back(-noise(90));
    }
    const keys::Analysis a = keys::analyze(durations);
    assert(a.confident);
    assert(a.bits == bits);
    assert(a.hex == "A9C1D593");
    assert(a.repeats == 3);
    assert(std::abs(a.te_us - 500) <= 40);
}

} // namespace

int main()
{
    test_noise_tail();
    test_empty();
    test_pwm_with_sync();
    test_pwm_without_sync();
    test_odd_bit_count();
    test_noise();
    test_not_pwm();
    test_waveform();
    test_view();
    test_text();
    return 0;
}

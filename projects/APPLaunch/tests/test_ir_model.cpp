// SPDX-License-Identifier: MIT

#include "../main/ui/model/ir_model.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace ir;

namespace {

uint32_t g_seed = 12345;

int jitter(int range)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return static_cast<int>((g_seed >> 8) % static_cast<uint32_t>(2 * range + 1)) - range;
}

// Jitter plus the pulse stretch a demodulating receiver adds.
Durations distort(const Durations &clean, int range, int stretch)
{
    Durations out;
    for (int32_t value : clean) {
        const int32_t length = (value < 0 ? -value : value) + jitter(range) + (value > 0 ? stretch : -stretch);
        out.push_back(value < 0 ? -length : length);
    }
    return out;
}

Code make(const char *protocol, uint32_t address, uint32_t command)
{
    Code code;
    code.protocol = protocol;
    code.address = address;
    code.command = command;
    return code;
}

std::vector<uint32_t> to_mode2(const Durations &frame)
{
    std::vector<uint32_t> samples;
    for (int32_t value : frame)
        samples.push_back(value > 0 ? (kMode2Pulse | static_cast<uint32_t>(value))
                                    : (kMode2Space | static_cast<uint32_t>(-value)));
    return samples;
}

void test_nec_round_trip()
{
    const uint32_t pairs[][2] = {{0x04, 0x08}, {0x00, 0x00}, {0xFF, 0xFF}, {0x5A, 0xA5}, {0x80, 0x01}};
    for (const auto &pair : pairs) {
        const Durations clean = encode_nec(make("nec", pair[0], pair[1]));
        assert(clean.size() == 67);
        assert(clean.front() == 9000 && clean[1] == -4500 && clean.back() > 0);
        for (int round = 0; round < 20; ++round) {
            Code code;
            assert(decode_nec(distort(clean, 80, 60), code));
            assert(code.protocol == "nec" && !code.repeat);
            assert(code.address == pair[0] && code.command == pair[1]);
        }
    }
    // Bits go out LSB first: address 0x04 is space pattern 0,0,1,0,...
    const Durations frame = encode_nec(make("nec", 0x04, 0x08));
    assert(frame[3] == -560 && frame[5] == -560 && frame[7] == -1690 && frame[9] == -560);
}

void test_nec_extended()
{
    Code code;
    assert(decode_nec(distort(encode_nec(make("necx", 0xBE40, 0x12)), 60, 40), code));
    assert(code.protocol == "necx" && code.address == 0xBE40 && code.command == 0x12);
    // An extended address whose bytes happen to be inverses is plain NEC.
    assert(decode_nec(encode_nec(make("necx", 0xFB04, 0x08)), code));
    assert(code.protocol == "nec" && code.address == 0x04 && code.command == 0x08);
    assert(decode_nec(encode_nec(make("nec32", 0x1234, 0x5678)), code));
    assert(code.protocol == "nec32" && code.address == 0x1234 && code.command == 0x5678);
}

void test_nec_repeat_and_garbage()
{
    Code code;
    assert(decode_nec(distort(encode_nec_repeat(), 80, 60), code));
    assert(code.protocol == "nec" && code.repeat);
    assert(decode(encode_nec_repeat()).repeat);

    assert(!decode_nec({}, code));
    assert(!decode_nec({9000}, code));
    assert(!decode_nec({9000, -4500, 560}, code));
    assert(!decode_nec({2400, -600, 1200, -600, 600, -600, 1200, -600, 600}, code));
    Durations truncated = encode_nec(make("nec", 1, 2));
    truncated.resize(41);
    assert(!decode_nec(truncated, code));
    Durations bad_space = encode_nec(make("nec", 1, 2));
    bad_space[21] = -5000;
    assert(!decode_nec(bad_space, code));

    assert(!decode({500, -500, 500}).valid());
    const Code raw = decode({2400, -600, 1200, -600, 600, -600, 1200, -600, 600});
    assert(raw.protocol == "raw" && raw.command != 0);
}

void test_raw_hash()
{
    // A Sony-like frame and a second one that differs in one bit.
    const Durations a = {2400, -600, 1200, -600, 600, -600, 1200, -600, 600, -600, 600, -600, 1200, -600, 600};
    Durations b = a;
    b[4] = 1200;
    const uint32_t hash = raw_hash(a);
    for (int round = 0; round < 200; ++round) {
        assert(raw_hash(distort(a, 50, 0)) == hash);
        assert(raw_hash(distort(a, 30, 40)) == hash);
    }
    assert(raw_hash(b) != hash);
    Durations shorter = a;
    shorter.resize(13);
    assert(raw_hash(shorter) != hash);
    assert(decode(distort(a, 50, 0)).command == hash);
}

void test_frames()
{
    const Durations first = encode_nec(make("nec", 0x04, 0x08));
    const Durations repeat = encode_nec_repeat();

    FrameAssembler assembler;
    Durations frame;
    assert(!assembler.feed(kMode2Space | 50000u, frame));
    for (uint32_t sample : to_mode2(first)) assert(!assembler.feed(sample, frame));
    assert(assembler.pending());
    assert(assembler.feed(kMode2Space | 40000u, frame));
    assert(frame == first);
    // A split pulse is merged back together.
    assert(!assembler.feed(kMode2Pulse | 4000u, frame));
    assert(!assembler.feed(kMode2Pulse | 5000u, frame));
    assert(!assembler.feed(kMode2Space | 2250u, frame));
    assert(!assembler.feed(kMode2Pulse | 560u, frame));
    assert(!assembler.feed(kMode2Frequency | 38000u, frame));
    assert(assembler.feed(kMode2Timeout | 125000u, frame));
    assert(frame == repeat);
    assert(!assembler.pending());

    // Overflow drops the partial frame; flush ends a quiet one.
    assert(!assembler.feed(kMode2Pulse | 9000u, frame));
    assert(!assembler.feed(kMode2Overflow, frame));
    assert(!assembler.pending() && !assembler.flush(frame));
    assert(!assembler.feed(kMode2Pulse | 900u, frame));
    assert(!assembler.feed(kMode2Space | 800u, frame));
    assert(assembler.flush(frame));
    assert(frame == Durations{900});
}

void test_remote_file()
{
    Remote remote;
    remote.buttons[0].code = make("nec", 0x04, 0x08);
    remote.buttons[0].raw = encode_nec(remote.buttons[0].code);
    remote.buttons[3].code = make("necx", 0xBF40, 0x12);
    remote.buttons[3].carrier_hz = 36000;
    const Durations sony = {2400, -600, 1200, -600, 600, -600, 1200, -600, 600};
    remote.buttons[6].code = decode(sony);
    remote.buttons[6].raw = sony;
    remote.buttons[6].carrier_hz = 40000;

    const std::string text = serialize_remote(remote);
    Remote loaded;
    assert(parse_remote(text, loaded) == 3);
    for (std::size_t i = 0; i < kSlotCount; ++i) {
        const Button &want = remote.buttons[i];
        const Button &got = loaded.buttons[i];
        assert(got.learned() == want.learned());
        if (!want.learned()) continue;
        assert(got.code.protocol == want.code.protocol);
        assert(got.code.address == want.code.address && got.code.command == want.code.command);
        assert(got.carrier_hz == want.carrier_hz && got.raw == want.raw);
    }
    assert(serialize_remote(loaded) == text);

    const std::string messy = "\n# comment\nbogus nec 4 8 38000\npwr what 4 8 38000\npwr nec zz 8 38000\n"
                              "vol+ nec 4 2 38000 9000 x\nch- raw 0 ABCD 38000\nok nec 4 44 38000\r\n"
                              "src raw 0 ABCD 38000 900 -800 900";
    assert(parse_remote(messy, loaded) == 2);
    assert(loaded.buttons[5].code.command == 0x44 && loaded.buttons[4].raw.size() == 3);
    assert(!loaded.buttons[0].learned() && !loaded.buttons[1].learned() && !loaded.buttons[7].learned());
    assert(parse_remote("", loaded) == 0);

    assert(slot_index("vol-") == 2 && slot_index("nope") == -1);
    assert(remote_path("/home/pi", 3) == "/home/pi/ir/remote-3.txt");
}

void test_tx_buffer()
{
    Remote remote;
    remote.buttons[0].code = make("nec", 0x04, 0x08);
    const std::vector<uint32_t> nec = tx_buffer(button_durations(remote.buttons[0]));
    assert(nec.size() == 67 && nec.size() % 2 == 1);
    assert(nec[0] == 9000 && nec[1] == 4500 && nec.back() == 560);

    // Leading and trailing spaces go, split pulses merge, the result is odd.
    const std::vector<uint32_t> raw = tx_buffer({-5000, 900, 100, -800, -200, 700, -30000});
    assert((raw == std::vector<uint32_t>{1000, 1000, 700}));
    assert(tx_buffer({}).empty() && tx_buffer({-100, -200}).empty());
    assert(button_durations(Button{}).empty());

    Durations huge;
    for (int i = 0; i < 2000; ++i) huge.push_back(i % 2 ? -500 : 500);
    const std::vector<uint32_t> capped = tx_buffer(huge);
    assert(capped.size() == kMaxTxDurations && capped.size() % 2 == 1);

    remote.buttons[1].code = make("raw", 0, 1);
    remote.buttons[1].raw = {900, -800, 900};
    assert(button_durations(remote.buttons[1]) == remote.buttons[1].raw);
}

void test_display()
{
    assert(code_label(make("nec", 0x04, 0x08)) == "04:08");
    assert(code_label(make("necx", 0xBF40, 0x12)) == "BF40:12");
    assert(code_label(make("raw", 0, 0x1A2B3C4D)) == "1A2B3C4D");
    assert(code_label(Code{}).empty());
    assert(describe(make("nec", 0x04, 0x08)) == "addr 04  cmd 08");
    assert(describe(make("raw", 0, 0xAB)) == "raw 000000AB");
    assert(carrier_label(38000) == "38k" && carrier_label(36700) == "36.7k");

    const std::vector<uint8_t> columns = burst_columns({1000, -1000, 2000}, 40);
    assert(columns.size() == 40);
    for (int x = 0; x < 40; ++x) assert(columns[x] == ((x < 10 || x >= 20) ? 1 : 0));
    // A pulse narrower than a column still shows.
    const std::vector<uint8_t> thin = burst_columns({10, -100000, 10}, 20);
    assert(thin.front() == 1 && thin.back() == 1 && thin[10] == 0);
    assert(burst_columns({}, 10) == std::vector<uint8_t>(10, 0));
    assert(burst_columns({100}, 0).empty());
}

} // namespace

int main()
{
    test_nec_round_trip();
    test_nec_extended();
    test_nec_repeat_and_garbage();
    test_raw_hash();
    test_frames();
    test_remote_file();
    test_tx_buffer();
    test_display();
    std::printf("ir_model: all tests passed\n");
    return 0;
}

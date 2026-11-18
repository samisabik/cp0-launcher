// SPDX-License-Identifier: MIT

#include "../main/ui/model/pins_model.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace pins;

namespace {

// Drives SCL and SDA the way a 100 kHz master does: data changes 2 us after
// SCL falls, SCL is high for the second half of each 10 us bit.
struct Bus {
    std::vector<Edge> scl, sda;
    int64_t t = 1000000;
    bool scl_level = true, sda_level = true;

    void set_scl(bool level, int64_t at)
    {
        if (level != scl_level) scl.push_back({at, level});
        scl_level = level;
    }
    void set_sda(bool level, int64_t at)
    {
        if (level != sda_level) sda.push_back({at, level});
        sda_level = level;
    }
    void start()
    {
        // Also a repeated start: release SDA, raise SCL, then pull SDA low.
        set_sda(true, t + 2000);
        set_scl(true, t + 5000);
        set_sda(false, t + 7000);
        set_scl(false, t + 10000);
        t += 10000;
    }
    void bit(bool value)
    {
        set_sda(value, t + 2000);
        set_scl(true, t + 5000);
        set_scl(false, t + 10000);
        t += 10000;
    }
    void byte(unsigned value, bool nak = false)
    {
        for (int i = 7; i >= 0; --i) bit((value >> i) & 1u);
        bit(nak);
    }
    void stop()
    {
        set_sda(false, t + 2000);
        set_scl(true, t + 5000);
        set_sda(true, t + 7000);
        t += 20000;
    }
};

void test_i2c_write()
{
    Bus bus;
    bus.start();
    bus.byte(0x48 << 1);
    bus.byte(0x0F);
    bus.stop();
    const std::vector<I2cToken> tokens = decode_i2c(bus.scl, bus.sda, true, true);
    assert(tokens.size() == 6);
    assert(tokens[0].kind == I2cToken::Kind::Start);
    assert(tokens[1].kind == I2cToken::Kind::Address && tokens[1].value == 0x48 && !tokens[1].read);
    assert(tokens[2].kind == I2cToken::Kind::Ack);
    assert(tokens[3].kind == I2cToken::Kind::Data && tokens[3].value == 0x0F);
    assert(tokens[4].kind == I2cToken::Kind::Ack);
    assert(tokens[5].kind == I2cToken::Kind::Stop);
    assert(tokens[0].t_ns < tokens[5].t_ns);
    assert(format_i2c(tokens) == "S 48w a 0F a P");
    assert(i2c_clock_hz(bus.scl) == 100000);
}

void test_i2c_read_nak_and_restart()
{
    Bus bus;
    bus.start();
    bus.byte((0x48 << 1) | 1);
    bus.byte(0xA5, true);
    bus.stop();
    assert(format_i2c(decode_i2c(bus.scl, bus.sda, true, true)) == "S 48r a A5 n P");

    // Nobody answers: the address byte is not acknowledged.
    Bus absent;
    absent.start();
    absent.byte(0x50 << 1, true);
    absent.stop();
    assert(format_i2c(decode_i2c(absent.scl, absent.sda, true, true)) == "S 50w n P");

    // Register read: write the register, repeated start, read two bytes.
    Bus combined;
    combined.start();
    combined.byte(0x68 << 1);
    combined.byte(0x75);
    combined.start();
    combined.byte((0x68 << 1) | 1);
    combined.byte(0x12);
    combined.byte(0x34, true);
    combined.stop();
    assert(format_i2c(decode_i2c(combined.scl, combined.sda, true, true)) == "S 68w a 75 a Sr 68r a 12 a 34 n P");

    // A capture that begins mid-transfer shows nothing until the next start.
    Bus late;
    late.start();
    late.byte(0x20 << 1);
    const std::size_t scl_skip = late.scl.size() / 2;
    late.byte(0xFF);
    late.stop();
    late.t += 50000;
    late.start();
    late.byte(0x21 << 1);
    late.stop();
    const int64_t cut = late.scl[scl_skip].t_ns;
    bool scl0 = true, sda0 = true;
    const std::vector<Edge> scl = window(late.scl, true, cut, INT64_MAX, scl0);
    const std::vector<Edge> sda = window(late.sda, true, cut, INT64_MAX, sda0);
    const std::string text = format_i2c(decode_i2c(scl, sda, scl0, sda0));
    assert(text == "S 21w a P");

    assert(decode_i2c({}, {}, true, true).empty());
    assert(format_i2c({}).empty());
    assert(i2c_clock_hz({}) == 0);
}

void test_trace_columns()
{
    // 1 kHz square wave starting low, rising at 0.5 ms, 1.5 ms, ...
    std::vector<Edge> edges;
    for (int i = 0; i < 20; ++i) edges.push_back({500000 + static_cast<int64_t>(i) * 500000, i % 2 == 0});

    // 100 us per column over the first 2 ms.
    const std::vector<uint8_t> cols = trace_columns(edges, false, 0, 2000000, 20);
    assert(cols.size() == 20);
    for (int x = 0; x < 20; ++x) {
        const bool high = (x >= 5 && x < 10) || x >= 15;
        const bool edge = x == 5 || x == 10 || x == 15;
        assert(cols[x] == ((high ? kColumnHigh : 0) | (edge ? kColumnEdge : 0)));
    }

    // Starting inside the capture takes the level from the edge before it.
    const std::vector<uint8_t> later = trace_columns(edges, false, 700000, 200000, 4);
    assert((later == std::vector<uint8_t>{1, 1, 1, 1}));

    // Zoomed out so each column holds a whole period: every column has an edge.
    const std::vector<uint8_t> wide = trace_columns(edges, false, 0, 10000000, 10);
    for (uint8_t value : wide) assert(value & kColumnEdge);

    // No edges: a flat line at the initial level.
    assert((trace_columns({}, true, 0, 1000, 3) == std::vector<uint8_t>{1, 1, 1}));
    assert((trace_columns({}, false, 0, 1000, 3) == std::vector<uint8_t>{0, 0, 0}));
    assert(trace_columns(edges, false, 0, 1000, 0).empty());
    assert((trace_columns(edges, false, 0, 0, 2) == std::vector<uint8_t>{0, 0}));

    bool level = false;
    const std::vector<Edge> part = window(edges, false, 1000000, 2000000, level);
    assert(part.size() == 2 && part[0].t_ns == 1000000 && part[1].t_ns == 1500000);
    assert(level);
    assert(window(edges, true, 0, 400000, level).empty() && level);

    bool initial = false;
    std::vector<Edge> many = edges;
    trim(many, initial, 30);
    assert(many.size() == 20 && !initial);
    trim(many, initial, 10);
    assert(many.size() == 5 && many.front().t_ns == edges[15].t_ns);
    assert(initial == edges[14].level);
}

void test_config()
{
    std::vector<LineRef> lines = parse_lines("5,6");
    assert(lines.size() == 2 && lines[0].chip == 0 && lines[0].offset == 5 && lines[1].offset == 6);
    lines = parse_lines(" 0:17 , 1:3,22 ");
    assert(lines.size() == 3 && lines[1].chip == 1 && lines[1].offset == 3 && lines[2].offset == 22);
    lines = parse_lines("5,x,,-3,5,7:,:4,1e3,99999,6;7,2:9:1,12");
    assert(lines.size() == 2 && lines[0].offset == 5 && lines[1].offset == 12);
    lines = parse_lines("1,2,3,4,5,6");
    assert(lines.size() == kMaxLines && lines[3].offset == 4);
    assert(parse_lines("").empty() && parse_lines("gpio").empty() && parse_lines(",,,").empty());
    assert(parse_lines("40:1").empty() && parse_lines("0:512").empty());

    int scl = -1, sda = -1;
    assert(parse_i2c_pair("0,1", 2, scl, sda) && scl == 0 && sda == 1);
    assert(parse_i2c_pair(" 3 , 2 ", 4, scl, sda) && scl == 3 && sda == 2);
    scl = sda = -1;
    assert(!parse_i2c_pair("", 4, scl, sda));
    assert(!parse_i2c_pair("0", 4, scl, sda));
    assert(!parse_i2c_pair("0,0", 4, scl, sda));
    assert(!parse_i2c_pair("0,2", 2, scl, sda));
    assert(!parse_i2c_pair("0,1,2", 4, scl, sda));
    assert(!parse_i2c_pair("a,b", 4, scl, sda));
    assert(!parse_i2c_pair("0,1", 0, scl, sda));
    assert(scl == -1 && sda == -1);
}

void test_steps()
{
    assert(nice_step(1) == 1 && nice_step(2) == 2 && nice_step(3) == 5 && nice_step(5) == 5);
    assert(nice_step(6) == 10 && nice_step(11) == 20 && nice_step(21) == 50 && nice_step(51) == 100);
    assert(nice_step(0) == 1 && nice_step(-5) == 1);
    assert(nice_step(1000000) == 1000000 && nice_step(1000001) == 2000000);

    const int64_t expected[] = {10000,    20000,    50000,     100000,    200000,    500000,   1000000, 2000000,
                                5000000,  10000000, 20000000,  50000000,  100000000, 200000000, 500000000,
                                1000000000};
    int64_t step = kMinDivNs;
    for (std::size_t i = 0; i + 1 < sizeof(expected) / sizeof(expected[0]); ++i) {
        assert(step == expected[i]);
        step = next_step(step, true);
    }
    assert(step == kMaxDivNs && next_step(step, true) == kMaxDivNs);
    for (std::size_t i = sizeof(expected) / sizeof(expected[0]); i-- > 1;) {
        assert(step == expected[i]);
        step = next_step(step, false);
    }
    assert(step == kMinDivNs && next_step(step, false) == kMinDivNs);
    assert(next_step(1, true) == 20000 && next_step(INT64_MAX, false) == 500000000);

    assert(format_time(10000) == "10 us" && format_time(2000000) == "2 ms" && format_time(1000000000) == "1 s");
    assert(format_time(500) == "500 ns" && format_time(1500000) == "1.5 ms");
    assert(format_hz(100000) == "100 kHz" && format_hz(400000) == "400 kHz" && format_hz(950) == "950 Hz");
    assert(format_hz(1000000) == "1 MHz" && format_hz(9500) == "9.5 kHz");
}

void test_grid()
{
    int row = -1, column = -1;
    assert(grid_cell(0x08, row, column) && row == 0 && column == 8);
    assert(grid_cell(0x48, row, column) && row == 4 && column == 8);
    assert(grid_cell(0x77, row, column) && row == 7 && column == 7);
    assert(!grid_cell(0x07, row, column) && !grid_cell(0x78, row, column) && !grid_cell(-1, row, column));
    assert(grid_address(0, 7) == -1 && grid_address(0, 8) == 0x08 && grid_address(7, 7) == 0x77);
    assert(grid_address(7, 8) == -1 && grid_address(8, 0) == -1 && grid_address(0, 16) == -1);
    for (int address = kI2cFirst; address <= kI2cLast; ++address) {
        assert(grid_cell(address, row, column));
        assert(grid_address(row, column) == address);
    }

    ProbeMap map{};
    assert(count(map, Probe::Found) == 0 && found_list(map).empty());
    for (int address = kI2cFirst; address <= kI2cLast; ++address) map[address] = Probe::Empty;
    for (int address : {0x34, 0x48, 0x55, 0x6B}) map[address] = Probe::Found;
    map[0x50] = Probe::InUse;
    map[0x03] = Probe::Found; // outside the scanned range, never listed
    assert(found_list(map) == "34 48 55 6B");
    assert(count(map, Probe::Found) == 4 && count(map, Probe::InUse) == 1);
    assert(count(map, Probe::Empty) == 0x70 - 5);
}

} // namespace

int main()
{
    test_i2c_write();
    test_i2c_read_nak_and_restart();
    test_trace_columns();
    test_config();
    test_steps();
    test_grid();
    std::printf("pins_model: all tests passed\n");
    return 0;
}

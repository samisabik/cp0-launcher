// SPDX-License-Identifier: MIT

#include "../main/ui/model/sensors_model.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

using namespace sensors;

// Lines in the shape rtl_433 -F json prints.
static const char *kNexus =
    "{\"time\" : \"2026-11-18 21:40:02\", \"model\" : \"Nexus-TH\", \"id\" : 42, \"channel\" : 1, "
    "\"battery_ok\" : 1, \"temperature_C\" : 21.300, \"humidity\" : 55}";
static const char *kAcuriteF =
    "{\"time\" : \"2026-11-18 21:40:10\", \"model\" : \"Acurite-606TX\", \"id\" : 9, \"battery_ok\" : 1, "
    "\"temperature_F\" : 47.660, \"mic\" : \"CHECKSUM\"}";
static const char *kLacrosse =
    "{\"time\" : \"2026-11-18 21:40:31\", \"model\" : \"LaCrosse-TX141Bv3\", \"id\" : 7, \"channel\" : 2, "
    "\"battery_ok\" : 1, \"temperature_C\" : -2.400, \"test\" : \"No\", \"mic\" : \"CRC\"}";
static const char *kFineOffsetLow =
    "{\"time\" : \"2026-11-18 21:41:00\", \"model\" : \"Fineoffset-WH2\", \"id\" : 118, \"battery_ok\" : 0, "
    "\"temperature_C\" : 19.000, \"humidity\" : 60, \"mic\" : \"CRC\"}";
static const char *kRemote =
    "{\"time\" : \"2026-11-18 21:41:05\", \"model\" : \"Generic-Remote\", \"id\" : 51234, \"cmd\" : 14, "
    "\"tristate\" : \"0101ZZ\"}";

static bool near(double a, double b, double tolerance = 1e-6)
{
    return std::fabs(a - b) <= tolerance;
}

static void test_parse()
{
    Reading r = parse_line(kNexus);
    assert(r.valid && r.model == "Nexus-TH" && r.id == "42" && r.channel == "1");
    assert(r.has_temperature && near(r.temperature_c, 21.3));
    assert(r.has_humidity && r.humidity == 55);
    assert(r.battery_ok == 1);
    assert(r.time == "2026-11-18 21:40:02");

    r = parse_line(kAcuriteF);
    assert(r.valid && r.model == "Acurite-606TX" && r.id == "9" && r.channel.empty());
    assert(r.has_temperature && near(r.temperature_c, (47.66 - 32.0) * 5.0 / 9.0));
    assert(near(r.temperature_c, 8.7, 0.01));
    assert(!r.has_humidity);

    r = parse_line(kLacrosse);
    assert(r.valid && r.has_temperature && near(r.temperature_c, -2.4) && !r.has_humidity && r.channel == "2");

    r = parse_line(kFineOffsetLow);
    assert(r.valid && r.battery_ok == 0 && r.has_humidity && r.humidity == 60);

    r = parse_line(kRemote);
    assert(r.valid && !r.has_temperature && !r.has_humidity && r.battery_ok == -1);

    // Compact JSON without spaces, and a string id.
    r = parse_line("{\"model\":\"Oregon-THGR122N\",\"id\":\"a1\",\"channel\":3,\"temperature_C\":5.5,\"humidity\":99}");
    assert(r.valid && r.id == "a1" && r.channel == "3" && near(r.temperature_c, 5.5) && r.humidity == 99);

    for (const char *garbage :
         {"", "rtl_433 version 23.11", "{", "}", "{}", "not json at all", "{\"time\" : \"2026-11-18\"}",
          "{\"model\" : }", "{\"model\" : \"\"}", "{\"model\" : \"Trunc", "[1,2,3]",
          "{\"models\" : \"x\", \"temperature_C\" : 1}"}) {
        assert(!parse_line(garbage).valid);
    }
    // A value that is not a number leaves the field unset.
    r = parse_line("{\"model\" : \"X\", \"temperature_C\" : \"warm\", \"humidity\" : 140}");
    assert(r.valid && !r.has_temperature && !r.has_humidity);
}

static void test_table()
{
    Table table;
    assert(!table.add_line("garbage", 0.0));
    assert(!table.add_line(kRemote, 0.0)); // nothing to show on a dashboard
    assert(table.sensors().empty());

    assert(table.add_line(kNexus, 10.0));
    assert(table.add_line(kAcuriteF, 11.0));
    assert(table.add_line(kLacrosse, 12.0));
    assert(table.add_line(kFineOffsetLow, 13.0));
    assert(table.sensors().size() == 4);
    assert(table.sensors()[0].latest.model == "Nexus-TH");
    assert(table.sensors()[3].latest.battery_ok == 0);

    // A repeat within the history interval replaces the newest sample.
    assert(table.add_line("{\"model\" : \"Nexus-TH\", \"id\" : 42, \"channel\" : 1, \"temperature_C\" : 21.5}", 15.0));
    assert(table.sensors().size() == 4);
    const Sensor &nexus = table.sensors()[0];
    assert(nexus.count == 2 && near(nexus.last_seen, 15.0));
    assert(nexus.history().size() == 1 && near(nexus.history()[0], 21.5, 1e-4));
    // Fields the repeat lacked are kept.
    assert(nexus.latest.has_humidity && nexus.latest.humidity == 55 && nexus.latest.battery_ok == 1);

    // Same model and id on another channel is a different sensor.
    assert(table.add_line("{\"model\" : \"Nexus-TH\", \"id\" : 42, \"channel\" : 2, \"temperature_C\" : 3.0}", 16.0));
    assert(table.sensors().size() == 5);

    // History ring: one sample per interval, oldest dropped when full.
    Table ring;
    for (int i = 0; i < kHistorySize + 10; ++i) {
        char line[128];
        std::snprintf(line, sizeof(line), "{\"model\" : \"T\", \"id\" : 1, \"temperature_C\" : %d.0}", i);
        assert(ring.add_line(line, 100.0 + i * kHistoryIntervalS));
    }
    const std::vector<float> history = ring.sensors()[0].history();
    assert(history.size() == static_cast<std::size_t>(kHistorySize));
    assert(near(history.front(), 10.0) && near(history.back(), kHistorySize + 9.0));
    for (std::size_t i = 1; i < history.size(); ++i) assert(near(history[i] - history[i - 1], 1.0));

    // The table is bounded: the sensor heard longest ago makes room.
    Table full;
    for (std::size_t i = 0; i < kMaxSensors + 3; ++i) {
        char line[128];
        std::snprintf(line, sizeof(line), "{\"model\" : \"T\", \"id\" : %zu, \"temperature_C\" : 1.0}", i);
        assert(full.add_line(line, static_cast<double>(i)));
    }
    assert(full.sensors().size() == kMaxSensors);
    assert(full.sensors().front().latest.id == "3");
    full.clear();
    assert(full.sensors().empty());
}

static void test_format()
{
    assert(format_age(0.0) == "0s");
    assert(format_age(-5.0) == "0s");
    assert(format_age(12.9) == "12s");
    assert(format_age(59.0) == "59s");
    assert(format_age(60.0) == "1m");
    assert(format_age(185.0) == "3m");
    assert(format_age(3599.0) == "59m");
    assert(format_age(7200.0) == "2h");
    assert(format_age(4 * 86400.0) == "4d");

    assert(format_temperature(parse_line(kNexus)) == "21.3");
    assert(format_temperature(parse_line(kAcuriteF)) == " 8.7");
    assert(format_temperature(parse_line(kLacrosse)) == "-2.4");
    assert(format_temperature(parse_line(kRemote)) == "--.-");
    Reading r;
    r.has_temperature = true;
    r.temperature_c = -12.4;
    assert(format_temperature(r) == "-12");
    r.temperature_c = 104.2;
    assert(format_temperature(r) == "104");
    r.temperature_c = -9.96;
    assert(format_temperature(r) == "-10");
    r.temperature_c = 99.94;
    assert(format_temperature(r) == "99.9");
    for (double t = -60.0; t < 130.0; t += 0.07) {
        r.temperature_c = t;
        assert(format_temperature(r).size() <= 4);
    }

    assert(format_name(parse_line(kNexus), 40) == "nexus-th  id 42 ch 1");
    assert(format_name(parse_line(kNexus), 13) == "nexus-th 42/1");
    assert(format_name(parse_line(kNexus), 10) == "nexus 42/1");
    assert(format_name(parse_line(kAcuriteF), 40) == "acurite-606tx  id 9");
    assert(format_name(parse_line(kNexus), 12) == "nexus-t 42/1");
    assert(format_frequency(433920000u) == "433.92");
    assert(format_frequency(868300000u) == "868.30");
    assert(kFrequenciesHz[0] == 433920000u && kFrequenciesHz[1] == 868300000u);

    Table table;
    table.add_line(kFineOffsetLow, 1.0);
    assert(format_log_line(table.sensors()[0], "2026-11-18 21:41:00") ==
           "2026-11-18 21:41:00 Fineoffset-WH2 id 118 19.0C 60% batt low");
    Table other;
    other.add_line(kLacrosse, 1.0);
    assert(format_log_line(other.sensors()[0], "") == "LaCrosse-TX141Bv3 id 7 ch 2 -2.4C");

    assert(command(433920000u, "/run/x") == "( rtl_433 -f 433920000 -F json 2>/dev/null; echo '#exit' ) >> '/run/x'");
    assert(decode_command("/run/p.ook", "/run/x") ==
           "( rtl_433 -r '/run/p.ook' -F json 2>/dev/null; echo '#exit' ) >> '/run/x'");
    assert(std::string(source_name(Source::Cc1101)) == "cc1101");
    assert(std::string(source_name(Source::Sdr)) == "sdr");
    // The exit marker is not mistaken for a reading.
    assert(!parse_line(kExitMark).valid);
}

static void test_sparkline()
{
    assert(sparkline({}, 10, 5) == std::vector<int>(10, -1));
    assert(sparkline({1.0f}, 0, 5).empty());

    // Rising 0..4 over 5 rows: bottom-left to top-right, right aligned.
    const std::vector<int> rising = sparkline({0.0f, 1.0f, 2.0f, 3.0f, 4.0f}, 8, 5);
    assert((rising == std::vector<int>{-1, -1, -1, 4, 3, 2, 1, 0}));

    // More history than columns keeps the newest samples.
    const std::vector<int> cut = sparkline({9.0f, 9.0f, 0.0f, 4.0f}, 2, 5);
    assert((cut == std::vector<int>{4, 0}));

    // A flat history sits in the middle instead of dividing by zero.
    const std::vector<int> flat = sparkline({20.0f, 20.0f, 20.0f}, 3, 9);
    assert((flat == std::vector<int>{4, 4, 4}));

    // A variation under one degree is not blown up to full height.
    const std::vector<int> small = sparkline({20.0f, 20.2f}, 2, 11);
    assert(small[0] - small[1] == 2);
    for (int row : sparkline({-40.0f, 60.0f, 3.0f, 3.1f}, 4, 16)) assert(row >= 0 && row < 16);
}

int main()
{
    test_parse();
    test_table();
    test_format();
    test_sparkline();
    std::puts("sensors_model: ok");
    return 0;
}

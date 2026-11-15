// SPDX-License-Identifier: MIT

#include "../main/ui/model/spectrum_model.hpp"

#include <algorithm>
#include <cassert>
#include <vector>
#include <string>

using namespace spectrum;

int main()
{
    // Datasheet example: 433.92 MHz with a 26 MHz crystal is FREQ 0x10B071.
    assert(freq_word(433920000) == 0x10B071);
    assert(freq_word(868000000) == 0x216276);

    const Band &b433 = kBands[1];
    assert(bin_frequency(b433, 0) == b433.start_hz);
    assert(bin_frequency(b433, kBins - 1) == b433.stop_hz);
    assert(bin_frequency(b433, -5) == b433.start_hz);
    assert(bin_frequency(b433, kBins + 5) == b433.stop_hz);

    // Every band stays inside the CC1101 synthesizer ranges.
    for (const Band &band : kBands) {
        const bool low = band.start_hz >= 300000000 && band.stop_hz <= 348000000;
        const bool mid = band.start_hz >= 387000000 && band.stop_hz <= 464000000;
        const bool high = band.start_hz >= 779000000 && band.stop_hz <= 928000000;
        assert(low || mid || high);
    }

    assert(rssi_dbm(0x00) == -74);
    assert(rssi_dbm(0x80) == -138);
    assert(rssi_dbm(0xFF) == -74);
    assert(rssi_dbm(0x14) == -64);

    assert(level_index(-200) == 0);
    assert(level_index(kMinDbm) == 0);
    assert(level_index(kMaxDbm) == kLevels - 1);
    assert(level_index(0) == kLevels - 1);
    assert(level_color(0) == 0x000000);
    assert(level_color(kLevels - 1) == 0xFFFFFF);
    assert(level_color(12) == 0x000000);
    assert(level_color(46) == 0x3DDC84);

    assert(format_mhz(433920000) == "433.920");
    assert(format_mhz(315000000) == "315.000");

    const std::string header = csv_header("2026-11-14T23:00:00");
    assert(header.find("band 868 start_hz=863000000 stop_hz=870000000 bins=160") != std::string::npos);
    assert(header.find("timestamp_ms,band,b0,b1,") != std::string::npos);
    assert(header.find(",b159\n") != std::string::npos);

    const int8_t values[3] = {-100, -64, -41};
    assert(csv_row(1700000000123, kBands[0], values, 3) == "1700000000123,315,-100,-64,-41\n");
    const std::vector<int32_t> raw = {-50, 300, 20, -700, 0, -100, 350, -9000};
    const std::vector<int32_t> norm = normalize_durations(raw);
    assert((norm == std::vector<int32_t>{320, -800, 350, -9000}));

    const std::string ook = ook_packet(raw, 315000000, "2026-11-14 23:30:00", -61);
    assert(ook.find(";ook 2 pulses\n;freq1 315000000\n;rssi -61.0 dB\n320 800\n350 9000\n;end\n") !=
           std::string::npos);
    assert(ook_packet({500}, 433920000, "t", -70).find("500 0\n;end") != std::string::npos);
    assert(ook_packet({500, -250000}, 433920000, "t", -70).find("500 100000\n") != std::string::npos);
    assert(ook_file_header("x").rfind(";pulse data\n;version 1\n;timescale 1us\n", 0) == 0);

    assert(sub_file_header(433920000) ==
           "Filetype: Flipper SubGhz RAW File\nVersion: 1\nFrequency: 433920000\n"
           "Preset: FuriHalSubGhzPresetOok650Async\nProtocol: RAW\n");
    assert(sub_raw_lines(raw) == "RAW_Data: 320 -800 350 -9000\n");
    std::vector<int32_t> many;
    for (int i = 0; i < 600; ++i) many.push_back(i % 2 ? -100 : 100);
    const std::string lines = sub_raw_lines(many);
    assert(lines.find("\nRAW_Data: 100") != std::string::npos);
    assert(std::count(lines.begin(), lines.end(), '\n') == 2);
    return 0;
}

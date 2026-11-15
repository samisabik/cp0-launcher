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

    assert(tx_allowed(433920000));
    assert(tx_allowed(433050000));
    assert(tx_allowed(434790000));
    assert(tx_allowed(868000000));
    assert(!tx_allowed(315000000));
    assert(!tx_allowed(433000000));
    assert(!tx_allowed(434800000));
    assert(!tx_allowed(862000000));

    const std::string sub = sub_file_header(433920000) + sub_raw_lines({320, -800, 350, -9000});
    const Capture round_trip = parse_sub(sub);
    assert(round_trip.freq_hz == 433920000);
    assert((round_trip.durations == std::vector<int32_t>{320, -800, 350, -9000}));
    // Multiple RAW_Data lines concatenate in order; a CRLF file still parses.
    const Capture multi = parse_sub("Frequency: 868000000\r\nRAW_Data: 100 -200\r\nRAW_Data: 300 -400\r\n");
    assert(multi.freq_hz == 868000000);
    assert((multi.durations == std::vector<int32_t>{100, -200, 300, -400}));
    assert(parse_sub("Protocol: RAW\n").durations.empty());

    // Finder channels are synthesizable by the CC1101 and tagged with a valid
    // band index for their capture floor.
    for (const FinderChannel &channel : kFinderChannels) {
        const bool low = channel.hz >= 300000000 && channel.hz <= 348000000;
        const bool mid = channel.hz >= 387000000 && channel.hz <= 464000000;
        const bool high = channel.hz >= 779000000 && channel.hz <= 928000000;
        assert(low || mid || high);
        assert(channel.band < kBands.size());
    }

    // Detector: quiet warm-up, one hit per burst, re-arm after quiet.
    FinderDetector detector;
    int8_t quiet[FinderDetector::kChannels];
    for (int8_t &value : quiet) value = -100;
    uint32_t t = 0;
    for (int i = 0; i < 40; ++i, t += 15) assert(!detector.update(quiet, t).hit);
    assert(detector.warmed_up());
    assert(detector.floor_dbm(3) == -100);

    int8_t burst[FinderDetector::kChannels];
    for (int i = 0; i < FinderDetector::kChannels; ++i) burst[i] = quiet[i];
    burst[3] = -50; // 433.92
    burst[2] = -80; // leakage into the neighbour
    const FinderHit hit = detector.update(burst, t += 15);
    assert(hit.hit && hit.channel == 3 && hit.rssi_dbm == -50);
    // Still transmitting: no second hit, and the floor is not dragged up.
    for (int i = 0; i < 10; ++i) assert(!detector.update(burst, t += 15).hit);
    assert(detector.floor_dbm(3) == -100);
    // Quiet long enough re-arms; the next burst is a new hit.
    for (int i = 0; i < 20; ++i) assert(!detector.update(quiet, t += 15).hit);
    assert(detector.update(burst, t += 15).hit);
    // A carrier that never stops re-triggers after the hold time.
    bool again = false;
    for (int i = 0; i < 120 && !again; ++i) again = detector.update(burst, t += 15).hit;
    assert(again);

    // Too weak in absolute terms is ignored even if it clears the margin.
    FinderDetector deaf;
    int8_t low[FinderDetector::kChannels];
    for (int8_t &value : low) value = -120;
    for (int i = 0; i < 30; ++i) deaf.update(low, i * 15);
    low[0] = -100;
    assert(!deaf.update(low, 1000).hit);
    return 0;
}

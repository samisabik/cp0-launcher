// SPDX-License-Identifier: MIT

#include "../main/ui/model/spectrum_model.hpp"

#include <algorithm>
#include <cassert>
#include <vector>
#include <string>

using namespace spectrum;

void test_capture_name()
{
    assert(spectrum::capture_name("Door Bell") == "door-bell");
    assert(spectrum::capture_name("gate/../x") == "gatex");
    assert(spectrum::capture_name("  car key  ") == "car-key");
    assert(spectrum::capture_name("abcdefghijklmnopq").size() == spectrum::kCaptureNameMax);
    assert(spectrum::capture_name("...").empty());
}

int main()
{
    test_capture_name();
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

    assert(format_mhz(433920000) == "433.920");
    assert(format_mhz(315000000) == "315.000");

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
    assert(tx_allowed(868000000));
    assert(tx_allowed(315000000)); // CC1101 300-348 band
    assert(tx_allowed(390000000)); // 387-464 band
    assert(tx_allowed(434800000));
    assert(tx_allowed(915000000)); // 779-928 band
    assert(!tx_allowed(200000000)); // below the lowest band
    assert(!tx_allowed(360000000)); // gap between 348 and 387
    assert(!tx_allowed(600000000)); // gap between 464 and 779
    assert(!tx_allowed(1000000000)); // above 928

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

    // Detector: fixed absolute trigger threshold, smoothed, debounced.
    FinderDetector detector;
    detector.configure(3, -50);
    int8_t quiet3[3] = {-90, -78, -92};
    uint32_t t = 0;
    for (int i = 0; i < 20; ++i) assert(!detector.update(quiet3, t += 15).hit);
    assert(detector.smoothed_dbm(1) <= -76 && detector.smoothed_dbm(1) >= -80);
    // Busy-band ambient below the trigger never fires, however long it holds.
    int8_t bump[3] = {-90, -66, -92};
    for (int i = 0; i < 8; ++i) assert(!detector.update(bump, t += 15).hit);
    // A press above the trigger fires once the smoothed reading clears it for
    // kHitPasses consecutive passes.
    int8_t press[3] = {-90, -21, -92};
    assert(!detector.update(press, t += 15).hit);
    assert(!detector.update(press, t += 15).hit);
    const FinderHit hit = detector.update(press, t += 15);
    assert(hit.hit && hit.slot == 1 && hit.rssi_dbm == -21);
    // Held press: no second hit while disarmed.
    for (int i = 0; i < 10; ++i) assert(!detector.update(press, t += 15).hit);
    // Quiet re-arms; next sustained press is a new hit.
    for (int i = 0; i < 40; ++i) assert(!detector.update(quiet3, t += 15).hit);
    assert(!detector.update(press, t += 15).hit);
    assert(!detector.update(press, t += 15).hit);
    assert(detector.update(press, t += 15).hit);

    // A single-pass spike never fires (needs kHitPasses consecutive passes).
    FinderDetector spiky;
    spiky.configure(3, -50);
    int8_t base[3] = {-90, -80, -92};
    for (int i = 0; i < 20; ++i) spiky.update(base, i * 15);
    int8_t spike[3] = {-90, -20, -92};
    assert(!spiky.update(spike, 1000).hit);
    assert(!spiky.update(base, 1015).hit);

        return 0;
}

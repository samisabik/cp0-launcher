// SPDX-License-Identifier: MIT

#include "../main/ui/model/sdr_model.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string>

using namespace sdr;

void test_iq_spectrum()
{
    // A tone 300 kHz above the tuner lands 128 bins right of DC at 2.4 MS/s.
    std::vector<uint8_t> iq(2 * sdr::kIqFft);
    const double f = 300000.0 / sdr::kIqRate;
    for (int i = 0; i < sdr::kIqFft; ++i) {
        iq[2 * i] = static_cast<uint8_t>(127.5 + 100.0 * std::cos(2 * 3.14159265358979 * f * i));
        iq[2 * i + 1] = static_cast<uint8_t>(127.5 + 100.0 * std::sin(2 * 3.14159265358979 * f * i));
    }
    const std::vector<float> bins = sdr::iq_spectrum_db(iq.data(), iq.size());
    assert(bins.size() == static_cast<std::size_t>(sdr::kIqFft));
    const int peak = static_cast<int>(std::max_element(bins.begin(), bins.end()) - bins.begin());
    assert(std::abs(peak - (sdr::kIqFft / 2 + 128)) <= 1);
    assert(bins[peak] > sdr::median_db(bins) + 30.0f);

    // With no offset the view is centred on the tuner: +300 kHz of a 1.2 MHz span.
    const std::vector<float> cols = sdr::span_columns(bins, 1200000u, 240, 0);
    const int col = static_cast<int>(std::max_element(cols.begin(), cols.end()) - cols.begin());
    assert(std::abs(col - (120 + 60)) <= 1);
    // With the tune offset the same tone sits 450 kHz above the listening frequency.
    const std::vector<float> shifted = sdr::span_columns(bins, 1200000u, 240);
    const int scol = static_cast<int>(std::max_element(shifted.begin(), shifted.end()) - shifted.begin());
    assert(std::abs(scol - (120 + 90)) <= 1);
    assert(sdr::iq_cmd(433920000u, -1) == "rtl_sdr -f 434070000 -s 2400000 -");
}

// Zero crossings per second of the demodulated audio, after settling.
static double audio_tone_hz(sdr::Demod demod, double deviation_hz, double tone_hz)
{
    const int seconds_samples = static_cast<int>(sdr::kIqRate) / 2;
    std::vector<uint8_t> iq(2 * static_cast<std::size_t>(seconds_samples));
    double phase = 0.0;
    for (int n = 0; n < seconds_samples; ++n) {
        const double t = static_cast<double>(n) / sdr::kIqRate;
        // The wanted signal sits kTuneOffsetHz below the tuner.
        const double inst = -sdr::kTuneOffsetHz + deviation_hz * std::sin(2 * 3.14159265358979 * tone_hz * t);
        phase += 2 * 3.14159265358979 * inst / sdr::kIqRate;
        iq[2 * n] = static_cast<uint8_t>(127.5 + 100.0 * std::cos(phase));
        iq[2 * n + 1] = static_cast<uint8_t>(127.5 + 100.0 * std::sin(phase));
    }
    sdr::Demodulator demodulator(demod);
    std::vector<int16_t> audio;
    demodulator.process(iq.data(), iq.size(), audio);
    assert(audio.size() == static_cast<std::size_t>(sdr::kAudioRate / 2));
    int crossings = 0;
    const std::size_t settle = audio.size() / 5;
    for (std::size_t i = settle + 1; i < audio.size(); ++i)
        if ((audio[i - 1] < 0) != (audio[i] < 0)) ++crossings;
    const double seconds = static_cast<double>(audio.size() - settle) / sdr::kAudioRate;
    return crossings / 2.0 / seconds;
}

void test_demodulators()
{
    assert(std::fabs(audio_tone_hz(sdr::Demod::WFM, 75000.0, 1000.0) - 1000.0) < 30.0);
    assert(std::fabs(audio_tone_hz(sdr::Demod::NFM, 5000.0, 1000.0) - 1000.0) < 30.0);
}

int main()
{
    test_iq_spectrum();
    test_demodulators();
    assert(clamp_hz(0) == kMinHz);
    assert(clamp_hz(2000000000) == kMaxHz);
    assert(clamp_hz(433920000) == 433920000u);

    assert(format_mhz(88500000) == "88.500");
    assert(format_hz(433920000) == "433.920 MHz");
    assert(format_hz(500000) == "500 kHz");
    assert(gain_label(-1) == "auto");
    assert(gain_label(30) == "30 dB");

    assert(demod_label(Demod::WFM) == std::string("wfm"));
    assert(demod_label(Demod::AM) == std::string("am"));

    const std::string power = power_cmd(433920000, 2000000, 30, "/tmp/p.csv");
    assert(power.find("rtl_power -f 432920000:434920000:6250 -i 1 -g 30 '/tmp/p.csv'") != std::string::npos);
    // Auto gain omits -g.
    assert(power_cmd(100000000, 1000000, -1, "/x").find(" -g ") == std::string::npos);


    assert(decode_cmd(433920000, 40, "/tmp/d").find("rtl_433 -f 433920000 -F json -g 40 > '/tmp/d'") !=
           std::string::npos);

    const Detect none = parse_detect("No supported devices found.\n");
    assert(!none.present);
    const Detect found =
        parse_detect("Found 1 device(s):\n  0:  Realtek, RTL2838UHIDIR, SN: 1\n\nUsing device 0\n");
    assert(found.present && found.name.find("RTL2838") != std::string::npos);

    const PowerRow row = parse_power_row("2024-01-01, 12:00:00, 433000000, 434000000, 2000.00, 20, -40, -38, -52");
    assert(row.valid && row.low_hz == 433000000u && row.step_hz == 2000u);
    assert(row.db.size() == 3 && row.db[0] == -40 && row.db[2] == -52);
    assert(!parse_power_row("bad,line").valid);

    const std::vector<int> cols = power_columns(row, 6);
    assert(static_cast<int>(cols.size()) == 6 && cols.front() == -40 && cols.back() == -52);

    assert(level_index(-120, -110, -40, 64) == 0);
    assert(level_index(-40, -110, -40, 64) == 63);

    assert(summarize_433("{\"model\":\"Nexus-TH\",\"id\":42,\"temperature_C\":21.3,\"humidity\":55}") ==
           "Nexus-TH id 42 21.3C 55%");
    assert(summarize_433("{\"time\":\"x\"}").empty());
    assert(summarize_433("{\"model\":\"Car-Remote\",\"button\":2}") == "Car-Remote btn 2");
    return 0;
}

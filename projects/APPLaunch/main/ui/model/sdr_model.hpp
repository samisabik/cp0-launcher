/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Pure helpers for the RTL-SDR page: frequency maths, command building and
// output parsing, free of LVGL and process access so they can be unit-tested.
namespace sdr {

// R820T2 tuner range with the v5 direct-sampling HF path included.
constexpr uint32_t kMinHz = 100000;
constexpr uint32_t kMaxHz = 1750000000u;
// One tuner bandwidth, so rtl_power sweeps the span without retuning.
constexpr uint32_t kMaxSpanHz = 2400000u;

enum class Demod { WFM, NFM, AM };
const char *demod_label(Demod demod);

struct Preset {
    const char *label;
    uint32_t hz;
    Demod demod;
    uint32_t span_hz;
};

inline constexpr std::array<Preset, 6> kPresets = {{
    {"fm", 98000000u, Demod::WFM, 2000000u},
    {"air", 124000000u, Demod::AM, 1000000u},
    {"433", 433920000u, Demod::NFM, 2000000u},
    {"pmr", 446000000u, Demod::NFM, 1000000u},
    {"noaa", 162400000u, Demod::NFM, 1000000u},
    {"ads-b", 1090000000u, Demod::AM, 2000000u},
}};

// Tuning step sizes cycled with the step key.
inline constexpr std::array<uint32_t, 5> kSteps = {1000u, 5000u, 25000u, 100000u, 1000000u};

uint32_t clamp_hz(int64_t hz);

// "88.500" in MHz with three decimals; used for the big readout.
std::string format_mhz(uint32_t hz);
// "88.500 MHz" or, below 1 MHz, "500 kHz".
std::string format_hz(uint32_t hz);

// -1 gain means the dongle's automatic gain; otherwise whole dB.
std::string gain_label(int gain_db);

// Shell command lines run through the process API. outpath receives streamed
// output the page tails; it is quoted by the caller's directory, assumed safe.
std::string detect_cmd();
std::string decode_cmd(uint32_t hz, int gain_db, const std::string &outpath);

// rtl_test output -> is a device present, and its name if any.
struct Detect {
    bool present = false;
    std::string name;
};
Detect parse_detect(const std::string &output);

// Live scope: rtl_sdr streams 8-bit unsigned IQ at kIqRate into a FIFO and the
// page takes a kIqFft-point FFT of the newest block.
// The dongle tunes kTuneOffsetHz above the listening frequency so the
// RTL2832U's DC spike stays off the signal; the page shifts it back.
constexpr uint32_t kIqRate = 2400000u;
constexpr int kIqFft = 1024;
constexpr int kTuneOffsetHz = 150000;
constexpr int kAudioRate = 48000;
std::string iq_cmd(uint32_t center_hz, int gain_db);

// Power per FFT bin in dB relative to full scale, ordered low to high
// frequency (DC in the middle). The DC spike of the RTL2832U is replaced by
// its neighbours. `iq` must hold 2 * kIqFft bytes.
std::vector<float> iq_spectrum_db(const uint8_t *iq, std::size_t bytes);

// Max of the bins covering each of `width` columns across `span_hz` centred
// `offset_hz` below the tuner frequency (the listening frequency).
std::vector<float> span_columns(const std::vector<float> &bins, uint32_t span_hz, int width,
                                int offset_hz = kTuneOffsetHz);

// Streaming demodulator for 8-bit IQ at kIqRate from a tuner sitting
// kTuneOffsetHz above the wanted signal. Produces mono s16 audio at
// kAudioRate: shift to baseband, decimate by 10 to 240 kHz, then
// WFM (discriminator, 50 us de-emphasis), NFM or AM at 48 kHz.
class Demodulator {
public:
    explicit Demodulator(Demod demod) : demod_(demod) {}
    void process(const uint8_t *iq, std::size_t bytes, std::vector<int16_t> &out);

private:
    Demod demod_;
    double phase_re_ = 1.0, phase_im_ = 0.0;
    float acc1_re_ = 0.0f, acc1_im_ = 0.0f;
    int count1_ = 0;
    float acc2_re_ = 0.0f, acc2_im_ = 0.0f, acc2_ = 0.0f;
    int count2_ = 0;
    float prev_re_ = 1.0f, prev_im_ = 0.0f;
    float deemph_ = 0.0f;
    float dc_ = 0.0f;
};

// Median of the values: a robust noise-floor estimate for auto scaling.
float median_db(std::vector<float> values);

// dBm -> 0..levels-1 on the display scale.
int level_index(int db, int floor_db, int ceil_db, int levels);

// One rtl_433 JSON line -> "Nexus-TH  id 42 21.3C 55%" or empty if not a
// device record.
std::string summarize_433(const std::string &json);

} // namespace sdr

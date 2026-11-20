/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Pure data and arithmetic for the CC1101 spectrum page, kept free of LVGL and
// hardware access so it can be unit-tested on the host.
namespace spectrum {

constexpr uint32_t kCrystalHz = 26000000;
constexpr int kBins = 160;
constexpr int kMinDbm = -110;
constexpr int kMaxDbm = -40;
constexpr int kLevels = 64;

struct Band {
    const char *label;
    uint32_t start_hz;
    uint32_t stop_hz;
    // MDMCFG4 high nibble sets the receive filter width; the low nibble is the
    // data-rate exponent, which does not affect RSSI readings.
    uint8_t mdmcfg4;
};

// 58 kHz filter for the narrow 315/433 spans, 102 kHz for the wider 868 span
// so neighbouring bins overlap instead of leaving gaps.
inline constexpr std::array<Band, 3> kBands = {{
    {"315", 314000000, 316000000, 0xF7},
    {"433", 433050000, 434790000, 0xF7},
    {"868", 863000000, 870000000, 0xC7},
}};

// Value for the FREQ2..FREQ0 registers: f_carrier = f_xosc * FREQ / 2^16.
uint32_t freq_word(uint32_t hz);

uint32_t bin_frequency(const Band &band, int bin);

// RSSI register to dBm, using the datasheet's 74 dB offset for this data rate.
int rssi_dbm(uint8_t raw);

// 0 .. kLevels-1 position of a level on the display scale.
int level_index(int dbm);

std::string format_mhz(uint32_t hz);

// Capture formats. A capture is a list of signed durations in microseconds:
// positive while the demodulated signal is high (carrier on), negative while low.

// Merges same-sign neighbours, drops zeros and leading lows so the list starts
// high and alternates, as both file formats require.
std::vector<int32_t> normalize_durations(const std::vector<int32_t> &durations);

// rtl_433 "OOK pulse data" text, readable with `rtl_433 -r file.ook`.
std::string ook_file_header(const std::string &created);
std::string ook_packet(const std::vector<int32_t> &durations, uint32_t freq_hz,
                       const std::string &received, int rssi_dbm);

// Flipper Zero Sub-GHz RAW file.
std::string sub_file_header(uint32_t freq_hz);
std::string sub_raw_lines(const std::vector<int32_t> &durations);

// A saved capture ready for playback: a carrier frequency and the signed
// microsecond durations that drive the OOK transmitter.
struct Capture {
    uint32_t freq_hz = 0;
    std::vector<int32_t> durations;
};

// True where the CC1101 synthesiser can key a carrier (300-348, 387-464,
// 779-928 MHz). Regulatory limits on a given frequency are the operator's call.
bool tx_allowed(uint32_t hz);

// Capture names are file stems: lowercase letters, digits, '-' and '_', at
// most kCaptureNameMax characters. Spaces become '-'; anything else is dropped.
constexpr std::size_t kCaptureNameMax = 13;
std::string capture_name(const std::string &typed);

// Parse a Flipper Sub-GHz RAW file (the format sub_file_header/sub_raw_lines
// write) back into a frequency and signed durations.
Capture parse_sub(const std::string &contents);

// Frequency finder: a short list of channels that remotes, doorbells and
// sensors actually use, checked with a wide filter many times a second.
struct FinderChannel {
    uint32_t hz;
    std::size_t band; // index into kBands
};

// band is the index into kBands whose capture noise floor is reused when
// auto-capturing this channel; it is the chip band the channel sits in, not a
// narrow span. 390/418 share the mid band with 433.
inline constexpr std::array<FinderChannel, 10> kFinderChannels = {{
    {315000000, 0}, // US remotes, some TPMS
    {345000000, 0}, // US alarm sensors (Honeywell)
    {390000000, 1}, // older US/UK gate and alarm gear
    {418000000, 1}, // older UK remotes
    {433420000, 1}, // multi-channel remotes
    {433920000, 1}, // the common EU/worldwide remote channel
    {434420000, 1}, // multi-channel remotes
    {868350000, 2}, // EU sensors and alarms
    {868950000, 2},
    {869525000, 2}, // EU high-power alarm sub-band
}};

struct FinderHit {
    bool hit = false;
    int slot = -1; // index into the active channel list
    int rssi_dbm = kMinDbm;
};

// Fires when a channel's smoothed RSSI holds above a fixed absolute threshold
// for a few consecutive passes, the model the Flipper Zero frequency analyser
// uses. A near-field press (tens of dB above ambient) crosses the threshold; a
// busy band's ambient chatter does not. No adaptive floor to drift down and
// turn ordinary ISM traffic into a false hit.
class FinderDetector
{
public:
    static constexpr int kMaxChannels = static_cast<int>(kFinderChannels.size());
    static constexpr int kHitPasses = 3;
    // A strong signal held right next to the device; comfortably above the
    // busiest ISM ambient (around -66 dBm) and far below a pressed remote.
    static constexpr int kMinThresholdDbm = -90;
    static constexpr int kMaxThresholdDbm = -30;
    static constexpr int kDefaultThresholdDbm = -50;
    static constexpr uint32_t kQuietRearmMs = 400;
    static constexpr uint32_t kMaxHoldMs = 1500;

    void configure(int count, int threshold_dbm);
    FinderHit update(const int8_t *rssi, uint32_t now_ms);
    int smoothed_dbm(int slot) const;
    int threshold_dbm() const { return threshold_; }
    void reset();

private:
    int count_ = 0;
    int threshold_ = kDefaultThresholdDbm;
    bool seeded_ = false;
    bool armed_ = true;
    bool quiet_ = true;
    uint32_t hit_ms_ = 0;
    uint32_t quiet_since_ms_ = 0;
    std::array<float, kMaxChannels> smooth_{};
    std::array<int, kMaxChannels> hot_{};
};

} // namespace spectrum

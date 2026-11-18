/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Infrared remote logic for the IR page: LIRC mode2 samples to frames, NEC
// decoding and encoding, a hash for unknown protocols, the remote file format
// and the transmit buffer. No LVGL, no kernel headers.
namespace ir {

// Signed microseconds: positive is a pulse (carrier on), negative is a space.
using Durations = std::vector<int32_t>;

// Sample layout of LIRC_MODE_MODE2, copied from linux/lirc.h so the model
// builds on hosts without that header.
inline constexpr uint32_t kMode2ValueMask = 0x00FFFFFFu;
inline constexpr uint32_t kMode2TypeMask = 0xFF000000u;
inline constexpr uint32_t kMode2Space = 0x00000000u;
inline constexpr uint32_t kMode2Pulse = 0x01000000u;
inline constexpr uint32_t kMode2Frequency = 0x02000000u;
inline constexpr uint32_t kMode2Timeout = 0x03000000u;
inline constexpr uint32_t kMode2Overflow = 0x04000000u;

inline constexpr int32_t kFrameGapUs = 20000;
inline constexpr uint32_t kDefaultCarrierHz = 38000;
inline constexpr std::size_t kMinFrameDurations = 8;
// Upper bound on a transmit buffer. Unverified: linux/lirc.h does not state
// the kernel's limit, so this is a conservative odd count.
inline constexpr std::size_t kMaxTxDurations = 511;

inline constexpr std::size_t kSlotCount = 8;
inline constexpr std::array<const char *, kSlotCount> kSlotNames = {"pwr", "vol+", "vol-", "mute",
                                                                    "src", "ok",   "ch+",  "ch-"};
inline constexpr int kRemoteCount = 4;

struct Code {
    std::string protocol; // "nec", "necx", "nec32", "raw", or empty when nothing is decoded
    uint32_t address = 0;
    uint32_t command = 0; // for "raw" this is the timing hash
    bool repeat = false;

    bool valid() const
    {
        return !protocol.empty();
    }
};

// Collects mode2 samples into frames. A frame starts at the first pulse and
// ends before a space of kFrameGapUs or more, a timeout sample, or flush().
class FrameAssembler
{
public:
    // Returns true when `frame` was filled with a completed frame.
    bool feed(uint32_t sample, Durations &frame);
    // Ends the frame in progress, for when the device has been quiet.
    bool flush(Durations &frame);
    bool pending() const
    {
        return !current_.empty();
    }
    // Last carrier reported by a LIRC_MODE2_FREQUENCY sample, or 0.
    uint32_t measured_carrier() const
    {
        return carrier_;
    }

private:
    Durations current_;
    uint32_t carrier_ = 0;

    bool finish(Durations &frame);
};

// Splits a duration list at spaces of gap_us or more. Frames have no leading
// or trailing space.
std::vector<Durations> split_frames(const Durations &durations, int32_t gap_us = kFrameGapUs);

Durations encode_nec(const Code &code);
Durations encode_nec_repeat();
// Returns false when the frame is not NEC. A repeat frame sets code.repeat
// and leaves address and command at 0.
bool decode_nec(const Durations &frame, Code &code);

// Hash of a frame that survives timing jitter: each duration is reduced to
// shorter / same / longer than the previous duration of the same kind.
uint32_t raw_hash(const Durations &frame);

// NEC when it decodes, otherwise "raw" with the hash. Frames shorter than
// kMinFrameDurations give an invalid Code.
Code decode(const Durations &frame);

struct Button {
    Code code;
    uint32_t carrier_hz = kDefaultCarrierHz;
    Durations raw;

    bool learned() const
    {
        return code.valid();
    }
};

struct Remote {
    std::array<Button, kSlotCount> buttons;
};

int slot_index(const std::string &name);
std::string serialize_remote(const Remote &remote);
// Lines that do not parse are skipped. Returns the number of buttons read.
int parse_remote(const std::string &text, Remote &remote);
std::string remote_path(const std::string &home, int number);

// What to transmit for a button: clean protocol timings when the protocol is
// known, the captured timings otherwise.
Durations button_durations(const Button &button);
// Unsigned microsecond list for a LIRC_MODE_PULSE write: starts and ends with
// a pulse, alternates pulse and space, odd length. Empty when nothing can be
// sent.
std::vector<uint32_t> tx_buffer(const Durations &durations);

// One byte per column, 1 where a pulse covers any part of the column.
std::vector<uint8_t> burst_columns(const Durations &frame, int width);

std::string code_label(const Code &code);    // "04:08", "FB04:08", "1A2B3C4D"
std::string describe(const Code &code);      // "addr 04  cmd 08", "raw 1A2B3C4D"
std::string carrier_label(uint32_t hz);      // "38k"

} // namespace ir

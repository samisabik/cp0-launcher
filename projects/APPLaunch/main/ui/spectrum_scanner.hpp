/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "model/spectrum_model.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// CC1101 on the expansion header (SPI0 CS1). Receive-only: the scanner never
// issues the transmit strobe.
class Cc1101
{
public:
    ~Cc1101() { close(); }

    bool open(std::string &error);
    void close();
    bool sweep(const spectrum::Band &band, int8_t *dbm);

    // OOK receiver with the demodulated signal on GDO0 (header G15), using the
    // register set behind the Flipper "Ook650Async" preset named in .sub files.
    // mdmcfg4 high nibble selects the receive filter; test regs follow it.
    bool begin_capture(uint32_t hz, uint8_t mdmcfg4 = 0x17, uint8_t test2 = 0x81, uint8_t test1 = 0x35);
    void end_capture();
    int read_rssi_dbm();

    // OOK transmitter. GDO0 becomes the TX data input: the Pi drives G15 high
    // to key the carrier on, low to key it off. begin_transmit tri-states the
    // chip's GDO0 driver and enters TX; end_transmit restores the receiver.
    bool begin_transmit(uint32_t hz);
    void end_transmit();

    // Frequency finder: wide 812 kHz filter and calibration on every hop, so
    // channels in different bands can be visited back to back.
    bool begin_finder();
    int finder_read(uint32_t hz);

private:
    int fd_ = -1;
    int previous_power_ = -1;

    bool transfer(uint8_t *data, std::size_t length);
    bool strobe(uint8_t command);
    bool write_reg(uint8_t address, uint8_t value);
    bool write_burst(uint8_t address, const uint8_t *values, std::size_t length);
    int read_status(uint8_t address);
    bool wait_state(uint8_t state, int timeout_us);
    bool set_frequency(uint32_t hz);
    bool configure();
};

// Receive filter bandwidth options for capture, narrow to wide.
struct CaptureProfile {
    int khz;
    uint8_t mdmcfg4, test2, test1;
};
inline constexpr std::array<CaptureProfile, 3> kCaptureProfiles = {{
    {162, 0x97, 0x88, 0x31},
    {325, 0x57, 0x88, 0x31},
    {650, 0x17, 0x81, 0x35},
}};

struct CaptureStatus {
    bool active = false;
    uint32_t freq_hz = 0;
    int threshold_dbm = -80;
    int rssi_dbm = spectrum::kMinDbm;
    uint32_t packets = 0;
    uint32_t sequence = 0;
    std::vector<int32_t> last;
    std::string name;
    bool automatic = false;
};

struct FinderStatus {
    // Only the fitted antenna's channels are active.
    int count = 0;
    std::size_t antenna = 1;
    int threshold_dbm = spectrum::FinderDetector::kDefaultThresholdDbm;
    std::array<uint32_t, spectrum::kFinderChannels.size()> hz{};
    std::array<int8_t, spectrum::kFinderChannels.size()> rssi{};
    uint32_t sequence = 0;
    struct Hit {
        uint32_t hz = 0;
        int rssi_dbm = spectrum::kMinDbm;
        int64_t at_ms = 0;
    };
    // Most recent first.
    std::vector<Hit> hits;
};

struct SpectrumFrame {
    std::array<std::array<int8_t, spectrum::kBins>, spectrum::kBands.size()> dbm{};
    std::array<uint32_t, spectrum::kBands.size()> sequence{};
};

class SpectrumScanner
{
public:
    ~SpectrumScanner() { stop(); }

    bool start(std::string &error);
    void stop();

    // Finder hops over the common channels; Band sweeps the selected band.
    enum class Mode { Finder, Band };
    void set_mode(Mode mode) { mode_.store(mode); }
    Mode mode() const { return mode_.load(); }

    // With auto capture on, a finder hit starts a capture at that channel,
    // which ends once the transmitter has been quiet for a moment.
    void set_auto_capture(bool enabled) { auto_capture_.store(enabled); }
    bool auto_capture() const { return auto_capture_.load(); }
    FinderStatus finder_status();

    // Absolute RSSI (dBm) a channel must hold above to fire an auto-capture.
    void adjust_finder_threshold(int delta_db);
    int finder_threshold_dbm() const { return finder_threshold_.load(); }

    // Only the selected band is swept, so the antenna on the hat can match it.
    void set_band(std::size_t index) { band_.store(index < spectrum::kBands.size() ? index : 0); }
    std::size_t band() const { return band_.load(); }

    void set_paused(bool paused) { paused_.store(paused); }
    bool paused() const { return paused_.load(); }

    // Capture records demodulated pulse timings at one frequency to
    // ~/spectrum/capture-*.ook (rtl_433) and .sub (Flipper RAW).
    void toggle_capture(uint32_t hz);
    bool capturing() const { return capture_active_.load(); }
    CaptureStatus capture_status();

    // Listening holds a capture open on one frequency without writing files
    // to ~/spectrum; each packet is queued as rtl_433 pulse text instead.
    // 0 stops it. A capture that cannot be opened clears the request and
    // posts a message.
    void listen(uint32_t hz) { listen_hz_.store(hz); }
    bool listening() const { return listen_hz_.load() != 0; }
    std::vector<std::string> take_listen_packets();

    // Steps the capture receive filter 162 -> 325 -> 650 kHz and re-measures
    // the noise floors. Takes effect on the next capture, not one in progress.
    void cycle_capture_bandwidth();
    int capture_bandwidth_khz() const { return kCaptureProfiles[capture_bw_index_.load()].khz; }

    // Replay a saved .sub capture over the transmitter. The worker parses the
    // file, refuses frequencies outside the licence-exempt TX bands, and keys
    // the carrier on G15 with the recorded timings.
    void request_transmit(const std::string &path);
    bool transmitting() const { return transmit_active_.load(); }

    SpectrumFrame snapshot();
    std::string take_message();

private:
    Cc1101 radio_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<std::size_t> band_{1};
    std::mutex mutex_;
    SpectrumFrame frame_;
    std::string message_;

    struct Edge {
        int64_t ns;
        bool rising;
    };
    std::atomic<bool> capture_request_{false};
    std::atomic<uint32_t> capture_request_hz_{0};
    std::atomic<bool> capture_active_{false};
    std::atomic<int> threshold_dbm_{-80};
    std::atomic<int> capture_bw_index_{2};
    std::atomic<bool> floors_dirty_{false};
    CaptureStatus capture_;
    int line_fd_ = -1;
    std::FILE *ook_file_ = nullptr;
    std::FILE *sub_file_ = nullptr;
    std::vector<Edge> history_;
    std::vector<Edge> packet_;
    bool packet_open_ = false;
    int64_t packet_start_ns_ = 0;
    int64_t last_strong_ns_ = 0;
    int packet_peak_dbm_ = spectrum::kMinDbm;

    std::atomic<Mode> mode_{Mode::Finder};
    std::atomic<bool> auto_capture_{true};
    FinderStatus finder_;
    spectrum::FinderDetector detector_;
    std::atomic<int> finder_threshold_{spectrum::FinderDetector::kDefaultThresholdDbm};
    std::size_t finder_antenna_ = 99; // forces configure on the first pass
    int finder_threshold_applied_ = 0;
    bool finder_ready_ = false;
    bool floors_measured_ = false;
    std::array<int, spectrum::kBands.size()> capture_floor_{};
    bool auto_session_ = false;
    int64_t auto_started_ns_ = 0;
    int64_t last_packet_ns_ = 0;
    std::string capture_stem_;
    std::atomic<uint32_t> listen_hz_{0};
    uint32_t listen_active_hz_ = 0;
    std::vector<std::string> listen_packets_;
    void handle_listen();
    void measure_capture_floors();
    void finder_step();
    void check_auto_session();

    void handle_capture_request();
    // A threshold above kMinDbm skips the noise-floor sampling, so an
    // automatic capture starts listening at once.
    bool open_capture(uint32_t hz, int threshold_dbm = spectrum::kMinDbm, bool save = true);
    void close_capture(bool report);
    void capture_step();
    void finish_packet(int64_t now_ns);

    std::atomic<bool> transmit_request_{false};
    std::atomic<bool> transmit_active_{false};
    std::string transmit_path_;
    void handle_transmit_request();
    void run_transmit(const std::string &path);
    bool set_tx_line(bool high);

    void run();
    void post_message(const std::string &text);
};

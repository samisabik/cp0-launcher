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
    bool begin_capture(uint32_t hz);
    void end_capture();
    int read_rssi_dbm();

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

struct CaptureStatus {
    bool active = false;
    uint32_t freq_hz = 0;
    int threshold_dbm = -80;
    int rssi_dbm = spectrum::kMinDbm;
    uint32_t packets = 0;
    uint32_t sequence = 0;
    std::vector<int32_t> last;
    std::string name;
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

    // Only the selected band is swept, so the antenna on the hat can match it.
    void set_band(std::size_t index) { band_.store(index < spectrum::kBands.size() ? index : 0); }
    std::size_t band() const { return band_.load(); }

    void set_paused(bool paused) { paused_.store(paused); }
    bool paused() const { return paused_.load(); }

    // Recording streams every sweep to a CSV file in ~/spectrum.
    void toggle_recording() { record_request_.store(true); }
    bool recording() const { return recording_.load(); }
    int64_t recording_started_ms() const { return recording_started_ms_.load(); }

    // Capture records demodulated pulse timings at one frequency to
    // ~/spectrum/capture-*.ook (rtl_433) and .sub (Flipper RAW).
    void toggle_capture(uint32_t hz);
    bool capturing() const { return capture_active_.load(); }
    void adjust_threshold(int delta_db);
    CaptureStatus capture_status();

    SpectrumFrame snapshot();
    std::string take_message();

private:
    Cc1101 radio_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<std::size_t> band_{1};
    std::atomic<bool> record_request_{false};
    std::atomic<bool> recording_{false};
    std::atomic<int64_t> recording_started_ms_{0};
    std::mutex mutex_;
    SpectrumFrame frame_;
    std::string message_;
    std::FILE *file_ = nullptr;

    struct Edge {
        int64_t ns;
        bool rising;
    };
    std::atomic<bool> capture_request_{false};
    std::atomic<uint32_t> capture_request_hz_{0};
    std::atomic<bool> capture_active_{false};
    std::atomic<int> threshold_dbm_{-80};
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

    void handle_capture_request();
    bool open_capture(uint32_t hz);
    void close_capture(bool report);
    void capture_step();
    void finish_packet(int64_t now_ns);

    void run();
    void handle_record_request();
    void close_file(bool report);
    void post_message(const std::string &text);
};

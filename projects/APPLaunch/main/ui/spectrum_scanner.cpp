/*
 * SPDX-License-Identifier: MIT
 */

#include "spectrum_scanner.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#if __has_include(<linux/spi/spidev.h>)
#include <linux/spi/spidev.h>
#define CP0_SPECTRUM_HAS_SPIDEV 1
#else
#define CP0_SPECTRUM_HAS_SPIDEV 0
#endif
#if __has_include(<linux/gpio.h>)
#include <linux/gpio.h>
#define CP0_SPECTRUM_HAS_GPIO_V2 1
#else
#define CP0_SPECTRUM_HAS_GPIO_V2 0
#endif
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr const char *kSpiDevice = "/dev/spidev0.1";
// The expansion header's 5V OUT is switched by the I/O expander and exposed as an LED.
constexpr const char *kPowerSwitch = "/sys/class/leds/ext_5v_out/brightness";
constexpr uint32_t kSpiHz = 4000000;
// Time for the RSSI estimate to settle after entering RX with a 58-102 kHz filter.
constexpr int kRssiSettleUs = 300;

constexpr uint8_t SRES = 0x30, SCAL = 0x33, SRX = 0x34, SIDLE = 0x36, SPWD = 0x39;
constexpr uint8_t REG_FREQ2 = 0x0D, REG_MDMCFG4 = 0x10;
constexpr uint8_t STATUS_VERSION = 0x31, STATUS_RSSI = 0x34, STATUS_MARCSTATE = 0x35;
constexpr uint8_t STATE_IDLE = 0x01, STATE_RX = 0x0D;
constexpr uint8_t REG_IOCFG0 = 0x02;

// GDO0 of the hat reaches the header on G15.
constexpr const char *kGpioChip = "/dev/gpiochip0";
constexpr unsigned kGdo0Line = 15;
// Packet gate: a transmission starts when RSSI reaches the threshold and ends
// after this long below it (with 3 dB of hysteresis).
constexpr int64_t kPacketEndNs = 15000000;
constexpr int64_t kPacketMaxNs = 3000000000LL;
constexpr int64_t kPreTriggerNs = 10000000;
constexpr std::size_t kPacketMaxEdges = 8000;
// Real OOK transmissions carry at least 16 pulses; shorter bursts are noise.
constexpr std::size_t kPacketMinDurations = 32;
// The capture threshold starts this far above the measured noise floor; the
// 650 kHz capture filter sees a much higher floor than the sweep filters.
constexpr int kAutoThresholdMarginDb = 12;
constexpr int kFloorSamples = 50;
// Synthetic trailing gap when a packet ends on a high level.
constexpr int32_t kTrailingGapUs = 10000;

int64_t monotonic_ns()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

int read_power_switch()
{
    std::FILE *file = std::fopen(kPowerSwitch, "r");
    if (!file) return -1;
    int value = -1;
    if (std::fscanf(file, "%d", &value) != 1) value = -1;
    std::fclose(file);
    return value;
}

bool write_power_switch(int value)
{
    std::FILE *file = std::fopen(kPowerSwitch, "w");
    if (!file) return false;
    const bool ok = std::fprintf(file, "%d", value) > 0;
    return std::fclose(file) == 0 && ok;
}

// Marks that this app switched 5V OUT on, so a session that ended without
// cleanup (for example a launcher restart) does not leave the hat powered.
std::string power_marker_path()
{
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    return std::string(runtime && runtime[0] ? runtime : "/tmp") + "/cp0-spectrum-5v";
}

int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string local_stamp(const char *format)
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char text[32];
    std::strftime(text, sizeof(text), format, &local);
    return text;
}

} // namespace

bool Cc1101::open(std::string &error)
{
#if !CP0_SPECTRUM_HAS_SPIDEV
    error = "SPI is not available on this platform";
    return false;
#endif
    previous_power_ = read_power_switch();
    if (access(power_marker_path().c_str(), F_OK) == 0) previous_power_ = 0;
    if (previous_power_ == 0) {
        if (std::FILE *marker = std::fopen(power_marker_path().c_str(), "w")) std::fclose(marker);
    }
    if (previous_power_ <= 0 && !write_power_switch(1)) {
        error = "cannot enable 5V out";
        return false;
    }
    usleep(50000);

    fd_ = ::open(kSpiDevice, O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        error = std::string("cannot open ") + kSpiDevice;
        close();
        return false;
    }
#if CP0_SPECTRUM_HAS_SPIDEV
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = kSpiHz;
    ioctl(fd_, SPI_IOC_WR_MODE, &mode);
    ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
#endif

    strobe(SRES);
    usleep(1000);
    const int version = read_status(STATUS_VERSION);
    if (version != 0x14 && version != 0x04) {
        error = "no CC1101 on the expansion port";
        close();
        return false;
    }
    if (!configure()) {
        error = "CC1101 configuration failed";
        close();
        return false;
    }
    return true;
}

void Cc1101::close()
{
    if (fd_ >= 0) {
        strobe(SIDLE);
        strobe(SPWD);
        ::close(fd_);
        fd_ = -1;
    }
    if (previous_power_ == 0 && write_power_switch(0)) unlink(power_marker_path().c_str());
    previous_power_ = -1;
}

bool Cc1101::transfer(uint8_t *data, std::size_t length)
{
#if !CP0_SPECTRUM_HAS_SPIDEV
    (void)data;
    (void)length;
    return false;
#else
    spi_ioc_transfer message{};
    message.tx_buf = reinterpret_cast<uintptr_t>(data);
    message.rx_buf = reinterpret_cast<uintptr_t>(data);
    message.len = static_cast<uint32_t>(length);
    message.speed_hz = kSpiHz;
    message.bits_per_word = 8;
    return ioctl(fd_, SPI_IOC_MESSAGE(1), &message) >= 0;
#endif
}

bool Cc1101::strobe(uint8_t command)
{
    uint8_t data[1] = {command};
    return transfer(data, sizeof(data));
}

bool Cc1101::write_reg(uint8_t address, uint8_t value)
{
    uint8_t data[2] = {address, value};
    return transfer(data, sizeof(data));
}

bool Cc1101::write_burst(uint8_t address, const uint8_t *values, std::size_t length)
{
    uint8_t data[64];
    if (length + 1 > sizeof(data)) return false;
    data[0] = static_cast<uint8_t>(address | 0x40);
    std::memcpy(data + 1, values, length);
    return transfer(data, length + 1);
}

int Cc1101::read_status(uint8_t address)
{
    uint8_t data[2] = {static_cast<uint8_t>(address | 0xC0), 0};
    return transfer(data, sizeof(data)) ? data[1] : -1;
}

bool Cc1101::wait_state(uint8_t state, int timeout_us)
{
    for (int waited = 0; waited <= timeout_us; waited += 50) {
        if ((read_status(STATUS_MARCSTATE) & 0x1F) == state) return true;
        usleep(50);
    }
    return false;
}

bool Cc1101::set_frequency(uint32_t hz)
{
    const uint32_t word = spectrum::freq_word(hz);
    const uint8_t values[3] = {static_cast<uint8_t>(word >> 16), static_cast<uint8_t>(word >> 8),
                               static_cast<uint8_t>(word)};
    return write_burst(REG_FREQ2, values, sizeof(values));
}

bool Cc1101::configure()
{
    // Register values follow TI SmartRF Studio settings for an OOK receiver with
    // packet handling off; only the RSSI estimate is used.
    static const struct { uint8_t address, value; } kConfig[] = {
        {0x00, 0x2E}, // IOCFG2: GDO2 high impedance
        {0x02, 0x2E}, // IOCFG0: GDO0 high impedance
        {0x08, 0x32}, // PKTCTRL0: asynchronous serial mode, infinite length
        {0x0B, 0x06}, // FSCTRL1: IF frequency
        {0x0C, 0x00}, // FSCTRL0
        {0x10, 0xF7}, // MDMCFG4: set per band before each sweep
        {0x11, 0x83}, // MDMCFG3
        {0x12, 0x30}, // MDMCFG2: OOK, no sync word
        {0x13, 0x00}, // MDMCFG1
        {0x14, 0xF8}, // MDMCFG0
        {0x17, 0x30}, // MCSM1: return to IDLE after RX
        {0x18, 0x08}, // MCSM0: no auto-calibration, calibrated once per band
        {0x19, 0x16}, // FOCCFG
        {0x1A, 0x6C}, // BSCFG
        {0x1B, 0x03}, // AGCCTRL2
        {0x1C, 0x00}, // AGCCTRL1
        {0x1D, 0x91}, // AGCCTRL0
        {0x21, 0x56}, // FREND1
        {0x22, 0x11}, // FREND0
        {0x23, 0xE9}, // FSCAL3
        {0x24, 0x2A}, // FSCAL2
        {0x25, 0x00}, // FSCAL1
        {0x26, 0x1F}, // FSCAL0
        {0x2C, 0x88}, // TEST2: values for filters under 325 kHz
        {0x2D, 0x31}, // TEST1
        {0x2E, 0x09}, // TEST0
    };
    if (!strobe(SIDLE)) return false;
    for (const auto &entry : kConfig)
        if (!write_reg(entry.address, entry.value)) return false;
    return true;
}

bool Cc1101::sweep(const spectrum::Band &band, int8_t *dbm)
{
    if (fd_ < 0) return false;
    // Calibrate the synthesizer once at the band centre; the spans are small
    // enough that per-bin calibration is not needed for level readings.
    if (!strobe(SIDLE) || !write_reg(REG_MDMCFG4, band.mdmcfg4)) return false;
    if (!set_frequency(band.start_hz + (band.stop_hz - band.start_hz) / 2)) return false;
    strobe(SCAL);
    if (!wait_state(STATE_IDLE, 2000)) return false;

    for (int bin = 0; bin < spectrum::kBins; ++bin) {
        strobe(SIDLE);
        set_frequency(spectrum::bin_frequency(band, bin));
        strobe(SRX);
        wait_state(STATE_RX, 1000);
        usleep(kRssiSettleUs);
        const int raw = read_status(STATUS_RSSI);
        dbm[bin] = static_cast<int8_t>(raw < 0 ? spectrum::kMinDbm : spectrum::rssi_dbm(static_cast<uint8_t>(raw)));
    }
    strobe(SIDLE);
    return true;
}

bool Cc1101::begin_capture(uint32_t hz)
{
    if (fd_ < 0) return false;
    static const struct { uint8_t address, value; } kCapture[] = {
        {0x10, 0x17}, // MDMCFG4: 650 kHz receive filter
        {0x1B, 0x07}, // AGCCTRL2
        {0x1C, 0x00}, // AGCCTRL1
        {0x1D, 0x91}, // AGCCTRL0
        {0x21, 0xB6}, // FREND1
        {0x2C, 0x81}, // TEST2: values for filters over 325 kHz
        {0x2D, 0x35}, // TEST1
        {REG_IOCFG0, 0x0D}, // GDO0: asynchronous serial data output
    };
    if (!strobe(SIDLE)) return false;
    for (const auto &entry : kCapture)
        if (!write_reg(entry.address, entry.value)) return false;
    if (!set_frequency(hz)) return false;
    strobe(SCAL);
    if (!wait_state(STATE_IDLE, 2000)) return false;
    strobe(SRX);
    return wait_state(STATE_RX, 2000);
}

void Cc1101::end_capture()
{
    if (fd_ < 0) return;
    strobe(SIDLE);
    configure();
}

int Cc1101::read_rssi_dbm()
{
    const int raw = read_status(STATUS_RSSI);
    return raw < 0 ? spectrum::kMinDbm : spectrum::rssi_dbm(static_cast<uint8_t>(raw));
}

bool SpectrumScanner::start(std::string &error)
{
    if (running_.load()) return true;
    if (!radio_.open(error)) return false;
    running_.store(true);
    worker_ = std::thread([this] { run(); });
    return true;
}

void SpectrumScanner::stop()
{
    running_.store(false);
    if (worker_.joinable()) worker_.join();
    close_capture(false);
    close_file(false);
    radio_.close();
}

void SpectrumScanner::toggle_capture(uint32_t hz)
{
    capture_request_hz_.store(hz);
    capture_request_.store(true);
}

void SpectrumScanner::adjust_threshold(int delta_db)
{
    threshold_dbm_.store(std::clamp(threshold_dbm_.load() + delta_db, -105, -40));
}

CaptureStatus SpectrumScanner::capture_status()
{
    std::lock_guard<std::mutex> lock(mutex_);
    CaptureStatus status = capture_;
    status.active = capture_active_.load();
    status.threshold_dbm = threshold_dbm_.load();
    return status;
}

void SpectrumScanner::handle_capture_request()
{
    if (!capture_request_.exchange(false)) return;
    if (capture_active_.load())
        close_capture(true);
    else
        open_capture(capture_request_hz_.load());
}

bool SpectrumScanner::open_capture(uint32_t hz)
{
#if CP0_SPECTRUM_HAS_GPIO_V2
    const char *home = std::getenv("HOME");
    const std::string directory = std::string(home && home[0] ? home : "/tmp") + "/spectrum";
    if (mkdir(directory.c_str(), 0755) != 0 && errno != EEXIST) {
        post_message("cannot create ~/spectrum");
        return false;
    }
    const int chip = ::open(kGpioChip, O_RDWR | O_CLOEXEC);
    if (chip < 0) {
        post_message("cannot open gpiochip0");
        return false;
    }
    gpio_v2_line_request request{};
    request.offsets[0] = kGdo0Line;
    request.num_lines = 1;
    std::snprintf(request.consumer, sizeof(request.consumer), "cc1101-gdo0");
    request.config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING |
                           GPIO_V2_LINE_FLAG_EDGE_FALLING;
    request.event_buffer_size = 1024;
    const int result = ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &request);
    ::close(chip);
    if (result < 0) {
        post_message("G15 is busy");
        return false;
    }
    line_fd_ = request.fd;
    fcntl(line_fd_, F_SETFL, fcntl(line_fd_, F_GETFL) | O_NONBLOCK);

    if (!radio_.begin_capture(hz)) {
        ::close(line_fd_);
        line_fd_ = -1;
        radio_.end_capture();
        post_message("radio capture setup failed");
        return false;
    }

    std::vector<int> floor;
    for (int i = 0; i < kFloorSamples; ++i) {
        floor.push_back(radio_.read_rssi_dbm());
        usleep(3000);
    }
    std::nth_element(floor.begin(), floor.begin() + floor.size() / 2, floor.end());
    threshold_dbm_.store(std::clamp(floor[floor.size() / 2] + kAutoThresholdMarginDb, -105, -40));

    const std::string stem = directory + "/capture-" + local_stamp("%Y%m%d-%H%M%S") + "-" +
                             spectrum::format_mhz(hz);
    ook_file_ = std::fopen((stem + ".ook").c_str(), "w");
    sub_file_ = std::fopen((stem + ".sub").c_str(), "w");
    if (!ook_file_ || !sub_file_) {
        close_capture(false);
        post_message("cannot write capture files");
        return false;
    }
    std::fputs(spectrum::ook_file_header(local_stamp("%Y-%m-%d %H:%M:%S")).c_str(), ook_file_);
    std::fputs(spectrum::sub_file_header(hz).c_str(), sub_file_);
    std::fflush(ook_file_);
    std::fflush(sub_file_);

    history_.clear();
    packet_.clear();
    packet_open_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        capture_ = CaptureStatus{};
        capture_.freq_hz = hz;
        capture_.name = stem.substr(stem.rfind('/') + 1);
    }
    capture_active_.store(true);
    return true;
#else
    (void)hz;
    post_message("GPIO events are not available");
    return false;
#endif
}

void SpectrumScanner::close_capture(bool report)
{
    if (packet_open_) finish_packet(monotonic_ns());
    const bool was_active = capture_active_.exchange(false);
    if (line_fd_ >= 0) ::close(line_fd_);
    line_fd_ = -1;
    if (ook_file_) std::fclose(ook_file_);
    if (sub_file_) std::fclose(sub_file_);
    ook_file_ = nullptr;
    sub_file_ = nullptr;
    if (was_active) radio_.end_capture();
    if (report && was_active) {
        CaptureStatus status = capture_status();
        post_message("saved " + std::to_string(status.packets) + " pkts");
    }
}

void SpectrumScanner::finish_packet(int64_t now_ns)
{
    packet_open_ = false;
    std::vector<int32_t> durations;
    std::size_t start = 0;
    while (start < packet_.size() && !packet_[start].rising) ++start;
    for (std::size_t i = start + 1; i < packet_.size(); ++i) {
        const int64_t us = (packet_[i].ns - packet_[i - 1].ns) / 1000;
        const int32_t value = static_cast<int32_t>(std::min<int64_t>(us, 1000000));
        durations.push_back(packet_[i - 1].rising ? value : -value);
    }
    if (start < packet_.size()) {
        const Edge &last = packet_.back();
        const int32_t tail = static_cast<int32_t>(std::min<int64_t>((now_ns - last.ns) / 1000, 1000000));
        if (last.rising) {
            durations.push_back(tail);
            durations.push_back(-kTrailingGapUs);
        } else {
            durations.push_back(-tail);
        }
    }
    packet_.clear();
    durations = spectrum::normalize_durations(durations);
    if (durations.size() < kPacketMinDurations) return;

    if (ook_file_) {
        std::fputs(spectrum::ook_packet(durations, capture_.freq_hz, local_stamp("%Y-%m-%d %H:%M:%S"),
                                        packet_peak_dbm_).c_str(), ook_file_);
        std::fflush(ook_file_);
    }
    if (sub_file_) {
        std::fputs(spectrum::sub_raw_lines(durations).c_str(), sub_file_);
        std::fflush(sub_file_);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ++capture_.packets;
    ++capture_.sequence;
    capture_.last = std::move(durations);
}

void SpectrumScanner::capture_step()
{
#if CP0_SPECTRUM_HAS_GPIO_V2
    pollfd entry{line_fd_, POLLIN, 0};
    poll(&entry, 1, 2);
    gpio_v2_line_event events[64];
    for (;;) {
        const ssize_t count = read(line_fd_, events, sizeof(events));
        if (count <= 0) break;
        for (ssize_t i = 0; i < count / static_cast<ssize_t>(sizeof(events[0])); ++i) {
            const Edge edge{static_cast<int64_t>(events[i].timestamp_ns),
                            events[i].id == GPIO_V2_LINE_EVENT_RISING_EDGE};
            if (packet_open_)
                packet_.push_back(edge);
            else
                history_.push_back(edge);
        }
    }
#endif
    const int64_t now = monotonic_ns();
    const int rssi = radio_.read_rssi_dbm();
    const int threshold = threshold_dbm_.load();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        capture_.rssi_dbm = rssi;
    }

    if (!packet_open_) {
        while (!history_.empty() && history_.front().ns < now - kPreTriggerNs)
            history_.erase(history_.begin());
        if (rssi >= threshold) {
            packet_open_ = true;
            packet_start_ns_ = now;
            last_strong_ns_ = now;
            packet_peak_dbm_ = rssi;
            packet_.assign(history_.begin(), history_.end());
            history_.clear();
        }
        return;
    }
    packet_peak_dbm_ = std::max(packet_peak_dbm_, rssi);
    if (rssi >= threshold - 3) last_strong_ns_ = now;
    if (now - last_strong_ns_ > kPacketEndNs || now - packet_start_ns_ > kPacketMaxNs ||
        packet_.size() > kPacketMaxEdges)
        finish_packet(now);
}

SpectrumFrame SpectrumScanner::snapshot()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return frame_;
}

std::string SpectrumScanner::take_message()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string text;
    text.swap(message_);
    return text;
}

void SpectrumScanner::post_message(const std::string &text)
{
    std::lock_guard<std::mutex> lock(mutex_);
    message_ = text;
}

void SpectrumScanner::close_file(bool report)
{
    if (!file_) return;
    std::fclose(file_);
    file_ = nullptr;
    recording_.store(false);
    if (report) post_message("session saved");
}

void SpectrumScanner::handle_record_request()
{
    if (!record_request_.exchange(false)) return;
    if (file_) {
        close_file(true);
        return;
    }
    const char *home = std::getenv("HOME");
    const std::string directory = std::string(home && home[0] ? home : "/tmp") + "/spectrum";
    if (mkdir(directory.c_str(), 0755) != 0 && errno != EEXIST) {
        post_message("cannot create ~/spectrum");
        return;
    }
    const std::string name = "cc1101-" + local_stamp("%Y%m%d-%H%M%S") + ".csv";
    file_ = std::fopen((directory + "/" + name).c_str(), "w");
    if (!file_) {
        post_message("cannot write " + name);
        return;
    }
    const std::string header = spectrum::csv_header(local_stamp("%Y-%m-%dT%H:%M:%S"));
    std::fputs(header.c_str(), file_);
    recording_started_ms_.store(now_ms());
    recording_.store(true);
    post_message("rec ~/spectrum/" + name);
}

void SpectrumScanner::run()
{
    std::array<int8_t, spectrum::kBins> sweep{};
    while (running_.load()) {
        handle_record_request();
        handle_capture_request();
        if (capture_active_.load()) {
            capture_step();
            continue;
        }
        if (paused_.load()) {
            usleep(50000);
            continue;
        }
        const std::size_t index = band_.load();
        const spectrum::Band &band = spectrum::kBands[index];
        if (!radio_.sweep(band, sweep.data())) {
            post_message("radio read failed");
            usleep(200000);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            frame_.dbm[index] = sweep;
            ++frame_.sequence[index];
        }
        if (file_) {
            const std::string row = spectrum::csv_row(now_ms(), band, sweep.data(), sweep.size());
            if (std::fputs(row.c_str(), file_) < 0) {
                close_file(false);
                post_message("write failed, recording stopped");
            }
        }
    }
}

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

constexpr uint8_t SRES = 0x30, SCAL = 0x33, SRX = 0x34, STX = 0x35, SIDLE = 0x36, SPWD = 0x39;
constexpr uint8_t REG_FREQ2 = 0x0D, REG_MDMCFG4 = 0x10, REG_PATABLE = 0x3E;
// OOK output level for PATABLE index 1. 0xC0 is about +10 dBm, the 433 MHz
// limit and well inside the 868 MHz allowance.
constexpr uint8_t kTxPower = 0xC0;
// A saved frame is sent this many times with a gap, as fixed-code remotes do.
constexpr int kTxRepeats = 3;
constexpr int64_t kTxGapNs = 20000000;
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
// Settle time for the RSSI estimate after entering RX on a finder hop.
// RSSI needs a few ms after a retune with per-hop calibration to settle;
// sampling sooner (the old 250 us) read a meaningless floor.
constexpr int kFinderSettleUs = 3500;
// Automatic captures stop this long after the last packet, or after this long
// if nothing decodes at all.
constexpr int64_t kAutoQuietNs = 1200000000LL;
constexpr int64_t kAutoGiveUpNs = 2500000000LL;
constexpr int kAutoCaptureMarginDb = 10;
// Synthetic trailing gap when a packet ends on a high level.
constexpr int32_t kTrailingGapUs = 10000;
// Packets held for a listener that has stopped collecting them.
constexpr std::size_t kListenQueueMax = 32;

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

bool Cc1101::begin_capture(uint32_t hz, uint8_t mdmcfg4, uint8_t test2, uint8_t test1)
{
    if (fd_ < 0) return false;
    const struct { uint8_t address, value; } kCapture[] = {
        {0x10, mdmcfg4}, // MDMCFG4: receive filter bandwidth in the high nibble
        {0x1B, 0x07}, // AGCCTRL2
        {0x1C, 0x00}, // AGCCTRL1
        {0x1D, 0x91}, // AGCCTRL0
        {0x21, 0xB6}, // FREND1
        {0x2C, test2}, // TEST2: narrower filters need the under-325 kHz values
        {0x2D, test1}, // TEST1
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

bool Cc1101::begin_transmit(uint32_t hz)
{
    if (fd_ < 0) return false;
    if (!strobe(SIDLE)) return false;
    if (!set_frequency(hz)) return false;
    // OOK uses two PATABLE entries: index 0 keys the carrier off, index 1 on.
    const uint8_t pa[2] = {0x00, kTxPower};
    if (!write_burst(REG_PATABLE, pa, sizeof(pa))) return false;
    // Tri-state the chip's GDO0 driver so the Pi can drive it as the TX input.
    if (!write_reg(REG_IOCFG0, 0x2E)) return false;
    strobe(SCAL);
    if (!wait_state(STATE_IDLE, 2000)) return false;
    return strobe(STX);
}

void Cc1101::end_transmit()
{
    if (fd_ < 0) return;
    strobe(SIDLE);
    configure();
}

bool Cc1101::begin_finder()
{
    if (fd_ < 0) return false;
    // Same receiver as capture: a low, frequency-dependent noise floor that
    // actually tracks signals. The fast-hop config read ~-53 dBm everywhere.
    static const struct { uint8_t address, value; } kFinder[] = {
        {0x10, 0x17}, // MDMCFG4: 650 kHz receive filter
        {0x1B, 0x07}, // AGCCTRL2
        {0x1C, 0x00}, // AGCCTRL1
        {0x1D, 0x91}, // AGCCTRL0
        {0x21, 0xB6}, // FREND1
        {0x2C, 0x81}, // TEST2
        {0x2D, 0x35}, // TEST1
        {REG_IOCFG0, 0x2E},
    };
    if (!strobe(SIDLE)) return false;
    for (const auto &entry : kFinder)
        if (!write_reg(entry.address, entry.value)) return false;
    return true;
}
int Cc1101::finder_read(uint32_t hz)
{
    if (fd_ < 0) return spectrum::kMinDbm;
    strobe(SIDLE);
    set_frequency(hz);
    strobe(SCAL);
    wait_state(STATE_IDLE, 2000);
    strobe(SRX);
    wait_state(STATE_RX, 2000);
    usleep(kFinderSettleUs);
    int peak = read_rssi_dbm();
    for (int i = 0; i < 4; ++i) {
        usleep(700);
        peak = std::max(peak, read_rssi_dbm());
    }
    return peak;
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
    listen_active_hz_ = 0;
}

void SpectrumScanner::toggle_capture(uint32_t hz)
{
    capture_request_hz_.store(hz);
    capture_request_.store(true);
}

void SpectrumScanner::adjust_finder_threshold(int delta_db)
{
    finder_threshold_.store(std::clamp(finder_threshold_.load() + delta_db,
                                       spectrum::FinderDetector::kMinThresholdDbm,
                                       spectrum::FinderDetector::kMaxThresholdDbm));
}

void SpectrumScanner::cycle_capture_bandwidth()
{
    capture_bw_index_.store((capture_bw_index_.load() + 1) % static_cast<int>(kCaptureProfiles.size()));
    // The noise floor depends on the filter, so the finder re-measures it.
    floors_dirty_.store(true);
}

CaptureStatus SpectrumScanner::capture_status()
{
    std::lock_guard<std::mutex> lock(mutex_);
    CaptureStatus status = capture_;
    status.active = capture_active_.load();
    status.threshold_dbm = threshold_dbm_.load();
    status.automatic = auto_session_;
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

void SpectrumScanner::request_transmit(const std::string &path)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transmit_path_ = path;
    }
    transmit_request_.store(true);
}

void SpectrumScanner::handle_transmit_request()
{
    if (!transmit_request_.exchange(false)) return;
    std::string path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path = transmit_path_;
    }
    run_transmit(path);
    // Transmitting leaves the radio in its default configuration.
    finder_ready_ = false;
}

bool SpectrumScanner::set_tx_line(bool high)
{
#if CP0_SPECTRUM_HAS_GPIO_V2
    gpio_v2_line_values values{};
    values.mask = 1;
    values.bits = high ? 1 : 0;
    return ioctl(line_fd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &values) >= 0;
#else
    (void)high;
    return false;
#endif
}

void SpectrumScanner::run_transmit(const std::string &path)
{
#if CP0_SPECTRUM_HAS_GPIO_V2
    std::string contents;
    if (std::FILE *file = std::fopen(path.c_str(), "r")) {
        char buffer[4096];
        std::size_t read_count = 0;
        while ((read_count = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
            contents.append(buffer, read_count);
        std::fclose(file);
    } else {
        post_message("cannot read capture");
        return;
    }

    const spectrum::Capture capture = spectrum::parse_sub(contents);
    if (capture.durations.empty()) {
        post_message("empty capture");
        return;
    }
    if (!spectrum::tx_allowed(capture.freq_hz)) {
        post_message("tx blocked on this band");
        return;
    }

    const int chip = ::open(kGpioChip, O_RDWR | O_CLOEXEC);
    if (chip < 0) {
        post_message("cannot open gpiochip0");
        return;
    }
    gpio_v2_line_request request{};
    request.offsets[0] = kGdo0Line;
    request.num_lines = 1;
    std::snprintf(request.consumer, sizeof(request.consumer), "cc1101-tx");
    request.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    const int result = ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &request);
    ::close(chip);
    if (result < 0) {
        post_message("G15 is busy");
        return;
    }
    line_fd_ = request.fd;
    set_tx_line(false);

    if (!radio_.begin_transmit(capture.freq_hz)) {
        radio_.end_transmit();
        ::close(line_fd_);
        line_fd_ = -1;
        post_message("tx setup failed");
        return;
    }

    transmit_active_.store(true);
    post_message("sending " + spectrum::format_mhz(capture.freq_hz));

    // Busy-wait against an absolute schedule so pulse edges do not drift; the
    // worker owns the radio, so blocking here is fine for the short burst.
    int64_t target = monotonic_ns();
    for (int pass = 0; pass < kTxRepeats && running_.load(); ++pass) {
        for (int32_t value : capture.durations) {
            set_tx_line(value > 0);
            target += static_cast<int64_t>(value > 0 ? value : -value) * 1000;
            while (monotonic_ns() < target) {
            }
        }
        set_tx_line(false);
        target += kTxGapNs;
        while (monotonic_ns() < target) {
        }
    }
    set_tx_line(false);

    radio_.end_transmit();
    ::close(line_fd_);
    line_fd_ = -1;
    transmit_active_.store(false);
    post_message("sent");
#else
    (void)path;
    post_message("GPIO output is not available");
#endif
}

bool SpectrumScanner::open_capture(uint32_t hz, int threshold_dbm, bool save)
{
#if CP0_SPECTRUM_HAS_GPIO_V2
    const char *home = std::getenv("HOME");
    const std::string directory = std::string(home && home[0] ? home : "/tmp") + "/spectrum";
    if (save && mkdir(directory.c_str(), 0755) != 0 && errno != EEXIST) {
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

    const CaptureProfile profile = kCaptureProfiles[capture_bw_index_.load()];
    if (!radio_.begin_capture(hz, profile.mdmcfg4, profile.test2, profile.test1)) {
        ::close(line_fd_);
        line_fd_ = -1;
        radio_.end_capture();
        post_message("radio capture setup failed");
        return false;
    }

    if (threshold_dbm > spectrum::kMinDbm) {
        threshold_dbm_.store(std::clamp(threshold_dbm, -105, -40));
    } else {
        std::vector<int> floor;
        for (int i = 0; i < kFloorSamples; ++i) {
            floor.push_back(radio_.read_rssi_dbm());
            usleep(3000);
        }
        std::nth_element(floor.begin(), floor.begin() + floor.size() / 2, floor.end());
        threshold_dbm_.store(std::clamp(floor[floor.size() / 2] + kAutoThresholdMarginDb, -105, -40));
    }

    const std::string stem = directory + "/capture-" + local_stamp("%Y%m%d-%H%M%S") + "-" +
                             spectrum::format_mhz(hz);
    capture_stem_ = save ? stem : std::string();
    if (save) {
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
    }

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
    (void)threshold_dbm;
    (void)save;
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
    finder_ready_ = false;
    const bool automatic = auto_session_;
    auto_session_ = false;
    if (!was_active) return;
    const CaptureStatus status = capture_status();
    // A capture that decoded nothing leaves no files behind.
    if (status.packets == 0 && !capture_stem_.empty()) {
        unlink((capture_stem_ + ".ook").c_str());
        unlink((capture_stem_ + ".sub").c_str());
    }
    if (!report) return;
    if (status.packets == 0)
        post_message(automatic ? "nothing decoded at " + spectrum::format_mhz(status.freq_hz) : "nothing captured");
    else
        post_message("saved " + std::to_string(status.packets) + " pkts @" + spectrum::format_mhz(status.freq_hz));
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

    std::string ook;
    if (ook_file_ || listen_active_hz_)
        ook = spectrum::ook_packet(durations, capture_.freq_hz, local_stamp("%Y-%m-%d %H:%M:%S"), packet_peak_dbm_);
    if (ook_file_) {
        std::fputs(ook.c_str(), ook_file_);
        std::fflush(ook_file_);
    }
    if (sub_file_) {
        std::fputs(spectrum::sub_raw_lines(durations).c_str(), sub_file_);
        std::fflush(sub_file_);
    }
    last_packet_ns_ = monotonic_ns();
    std::lock_guard<std::mutex> lock(mutex_);
    if (listen_active_hz_ && listen_packets_.size() < kListenQueueMax) listen_packets_.push_back(std::move(ook));
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

void SpectrumScanner::handle_listen()
{
    const uint32_t hz = listen_hz_.load();
    if (hz == listen_active_hz_) return;
    if (capture_active_.load()) close_capture(false);
    listen_active_hz_ = hz;
    if (hz == 0 || open_capture(hz, spectrum::kMinDbm, false)) return;
    listen_active_hz_ = 0;
    // Left alone when the caller has asked for another frequency meanwhile.
    uint32_t expected = hz;
    listen_hz_.compare_exchange_strong(expected, 0);
}

std::vector<std::string> SpectrumScanner::take_listen_packets()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> packets;
    packets.swap(listen_packets_);
    return packets;
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

FinderStatus SpectrumScanner::finder_status()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return finder_;
}

// The capture filter sees a different noise floor than the finder, so each
// band's capture floor is measured once and reused for automatic captures.
void SpectrumScanner::measure_capture_floors()
{
    static constexpr uint32_t kCentres[] = {315000000, 433920000, 868350000};
    for (std::size_t band = 0; band < capture_floor_.size(); ++band) {
        capture_floor_[band] = -80;
        const CaptureProfile profile = kCaptureProfiles[capture_bw_index_.load()];
        if (!radio_.begin_capture(kCentres[band], profile.mdmcfg4, profile.test2, profile.test1)) continue;
        std::vector<int> samples;
        for (int i = 0; i < 20; ++i) {
            samples.push_back(radio_.read_rssi_dbm());
            usleep(3000);
        }
        std::nth_element(samples.begin(), samples.begin() + samples.size() / 2, samples.end());
        capture_floor_[band] = samples[samples.size() / 2];
    }
    radio_.end_capture();
    floors_measured_ = true;
}

void SpectrumScanner::finder_step()
{
    if (floors_dirty_.exchange(false)) floors_measured_ = false;
    if (!floors_measured_) measure_capture_floors();
    if (!finder_ready_) {
        if (!radio_.begin_finder()) {
            post_message("radio read failed");
            usleep(200000);
            return;
        }
        finder_ready_ = true;
    }

    // Active channels are those of the fitted antenna (the selected band).
    const std::size_t antenna = band_.load();
    const int threshold = finder_threshold_.load();
    std::array<uint32_t, spectrum::kFinderChannels.size()> active_hz{};
    int count = 0;
    for (const spectrum::FinderChannel &channel : spectrum::kFinderChannels)
        if (channel.band == antenna && count < static_cast<int>(active_hz.size()))
            active_hz[count++] = channel.hz;

    if (antenna != finder_antenna_ || threshold != finder_threshold_applied_) {
        finder_antenna_ = antenna;
        finder_threshold_applied_ = threshold;
        detector_.configure(count, threshold);
        std::lock_guard<std::mutex> lock(mutex_);
        finder_.hits.clear();
    }

    std::array<int8_t, spectrum::kFinderChannels.size()> rssi{};
    for (int i = 0; i < count; ++i) rssi[i] = static_cast<int8_t>(radio_.finder_read(active_hz[i]));
    const int64_t now_ns = monotonic_ns();
    const spectrum::FinderHit hit = detector_.update(rssi.data(), static_cast<uint32_t>(now_ns / 1000000));

    ++rate_passes_;
    int rate = -1;
    if (now_ns - rate_window_ns_ >= 1000000000LL) {
        rate = static_cast<int>(rate_passes_ * 1000000000LL / std::max<int64_t>(1, now_ns - rate_window_ns_));
        rate_window_ns_ = now_ns;
        rate_passes_ = 0;
    }
    const uint32_t hit_hz = hit.hit && hit.slot < count ? active_hz[hit.slot] : 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finder_.count = count;
        finder_.antenna = antenna;
        finder_.threshold_dbm = threshold;
        finder_.hz = active_hz;
        finder_.rssi = rssi;
        ++finder_.sequence;
        if (rate >= 0) finder_.passes_per_second = rate;
        if (hit_hz) {
            finder_.hits.insert(finder_.hits.begin(), {hit_hz, hit.rssi_dbm, now_ms()});
            if (finder_.hits.size() > 4) finder_.hits.resize(4);
        }
    }

    if (hit_hz && auto_capture_.load()) {
        if (open_capture(hit_hz, capture_floor_[antenna] + kAutoCaptureMarginDb)) {
            auto_session_ = true;
            auto_started_ns_ = monotonic_ns();
            last_packet_ns_ = 0;
        }
    }
}

void SpectrumScanner::check_auto_session()
{
    if (!auto_session_ || !capture_active_.load()) return;
    const int64_t now = monotonic_ns();
    const bool decoded = last_packet_ns_ > 0;
    if ((decoded && now - last_packet_ns_ > kAutoQuietNs) || (!decoded && now - auto_started_ns_ > kAutoGiveUpNs))
        close_capture(true);
}

void SpectrumScanner::run()
{
    std::array<int8_t, spectrum::kBins> sweep{};
    while (running_.load()) {
        handle_record_request();
        handle_listen();
        handle_capture_request();
        handle_transmit_request();
        if (capture_active_.load()) {
            capture_step();
            check_auto_session();
            continue;
        }
        if (paused_.load()) {
            usleep(50000);
            continue;
        }
        if (mode_.load() == Mode::Finder) {
            finder_step();
            continue;
        }
        if (finder_ready_) {
            radio_.end_capture();
            finder_ready_ = false;
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

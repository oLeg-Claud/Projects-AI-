#include "uso_source.hpp"
#include "dsp_internal.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <errno.h>
  #include <fcntl.h>
  #include <termios.h>
  #include <unistd.h>
  #include <sys/select.h>
#endif

namespace dsp {

/* ============================================================================
 * Shared Legacy Packet Parser (24-bit 5-byte & 32-bit 6-byte uco32)
 * Ported verbatim from YCO8_1.cpp lines 223-244.
 * ============================================================================ */
int32_t LegacySerialUsoSource::parse_packet(
    const uint8_t* packet,
    int32_t len,
    int32_t bit_mode,
    int32_t* out_sample
) {
    if (!packet || !out_sample) return DSP_ERR_INVALID_ARG;

    const int32_t expected_len = (bit_mode == 32) ? 6 : 5;
    if (len < expected_len) return DSP_ERR_INVALID_ARG;

    if (packet[0] != 0x00) return DSP_ERR_INVALID_ARG;

    const uint8_t trailer = packet[expected_len - 1];
    if (trailer != 0xFF && trailer != 0xFE) {
        return DSP_ERR_INVALID_ARG;
    }

    if (bit_mode == 32) {
        const uint32_t uval =
            (static_cast<uint32_t>(packet[1]) << 24) |
            (static_cast<uint32_t>(packet[2]) << 16) |
            (static_cast<uint32_t>(packet[3]) << 8)  |
            static_cast<uint32_t>(packet[4]);
        *out_sample = static_cast<int32_t>(uval);
    } else {
        /* 24-bit signed big-endian in bytes 1..3, arithmetic sign-extend */
        const uint32_t shifted =
            (static_cast<uint32_t>(packet[1]) << 24) |
            (static_cast<uint32_t>(packet[2]) << 16) |
            (static_cast<uint32_t>(packet[3]) << 8);
        *out_sample = static_cast<int32_t>(shifted) >> 8;
    }

    return (trailer == 0xFE) ? DSP_ERR_OVERFLOW : DSP_OK;
}

/* ============================================================================
 * Platform Serial Helpers
 * ============================================================================ */
namespace {

intptr_t open_serial_57600_8n1(const char* port_name, int32_t read_timeout_ms) {
    if (!port_name || port_name[0] == '\0') return -1;

#if defined(_WIN32)
    std::string full_name = port_name;
    if (full_name.rfind("\\\\.\\", 0) != 0) {
        full_name = "\\\\.\\" + full_name;
    }
    HANDLE h = CreateFileA(
        full_name.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) return -1;

    COMMTIMEOUTS cto{};
    if (!GetCommTimeouts(h, &cto)) { CloseHandle(h); return -1; }
    cto.ReadIntervalTimeout         = 0;
    cto.ReadTotalTimeoutMultiplier  = 0;
    cto.ReadTotalTimeoutConstant    = static_cast<DWORD>(read_timeout_ms);
    cto.WriteTotalTimeoutMultiplier = 0;
    cto.WriteTotalTimeoutConstant   = 20;
    if (!SetCommTimeouts(h, &cto)) { CloseHandle(h); return -1; }

    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(h, &dcb)) { CloseHandle(h); return -1; }
    dcb.BaudRate = CBR_57600;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    if (!SetCommState(h, &dcb)) { CloseHandle(h); return -1; }

    PurgeComm(h, PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR);
    return reinterpret_cast<intptr_t>(h);
#else
    (void)read_timeout_ms;
    int fd = ::open(port_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;

    struct termios tty{};
    if (tcgetattr(fd, &tty) != 0) {
        ::close(fd);
        return -1;
    }

    cfsetospeed(&tty, B57600);
    cfsetispeed(&tty, B57600);
    cfmakeraw(&tty);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | CRTSCTS);
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        ::close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return static_cast<intptr_t>(fd);
#endif
}

void close_serial_handle(intptr_t handle) {
    if (handle < 0) return;
#if defined(_WIN32)
    CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
    ::close(static_cast<int>(handle));
#endif
}

bool serial_write_byte(intptr_t handle, uint8_t b) {
    if (handle < 0) return false;
#if defined(_WIN32)
    DWORD written = 0;
    return WriteFile(reinterpret_cast<HANDLE>(handle), &b, 1, &written, nullptr) && written == 1;
#else
    ssize_t w = ::write(static_cast<int>(handle), &b, 1);
    return w == 1;
#endif
}

int32_t serial_read_bytes(intptr_t handle, uint8_t* dst, int32_t len, int32_t timeout_ms) {
    if (handle < 0 || !dst || len <= 0) return -1;
#if defined(_WIN32)
    (void)timeout_ms;
    DWORD got = 0;
    if (!ReadFile(reinterpret_cast<HANDLE>(handle), dst, static_cast<DWORD>(len), &got, nullptr)) {
        return -1;
    }
    return static_cast<int32_t>(got);
#else
    int fd = static_cast<int>(handle);
    int32_t total = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (total < len) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        auto rem_us = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv{};
        tv.tv_sec  = static_cast<long>(rem_us / 1000000);
        tv.tv_usec = static_cast<long>(rem_us % 1000000);
        int rv = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (rv < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (rv == 0) break;
        ssize_t n = ::read(fd, dst + total, static_cast<size_t>(len - total));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        total += static_cast<int32_t>(n);
    }
    return total;
#endif
}

} // namespace

/* ============================================================================
 * 1. LegacySerialUsoSource Implementation
 * ============================================================================ */
LegacySerialUsoSource::LegacySerialUsoSource()
    : handle_(-1), bit_mode_(32), gain_index_(0), synced_(false) {}

LegacySerialUsoSource::~LegacySerialUsoSource() {
    close();
}

int32_t LegacySerialUsoSource::probe_port(const char* port_name, int32_t* out_uso_id) {
    if (!port_name || !out_uso_id) return DSP_ERR_INVALID_ARG;
    *out_uso_id = 0;

    intptr_t h = open_serial_57600_8n1(port_name, 50);
    if (h < 0) return DSP_ERR_PORT_OPEN;

    int32_t status = DSP_ERR_NOT_FOUND;
    if (serial_write_byte(h, 0xAA)) {
        uint8_t ack = 0;
        if (serial_read_bytes(h, &ack, 1, 50) == 1 && ack == 0x55) {
            if (serial_write_byte(h, 0x66)) {
                uint8_t info[5]{};
                if (serial_read_bytes(h, info, 5, 80) == 5 &&
                    info[0] == 0x01 && info[4] == 0xFF) {
                    *out_uso_id = static_cast<int32_t>(info[1]);
                    status = DSP_OK;
                }
            }
        }
    }

    close_serial_handle(h);
    return status;
}

int32_t LegacySerialUsoSource::open(const DspConfig& cfg) {
    close();
    bit_mode_   = (cfg.uso_bit_mode == 24) ? 24 : 32;
    gain_index_ = cfg.gain_index;
    synced_     = false;

    handle_ = open_serial_57600_8n1(cfg.port_or_path, 1000);
    if (handle_ < 0) return DSP_ERR_PORT_OPEN;

    /* Hardware initialization sequence from YCO8_1.cpp lines 70-98 */
    const uint8_t gain_cmd = legacy_gain_command_byte(cfg.gain_index, bit_mode_);
    const uint8_t freq_cmd = legacy_freq_command_byte(cfg.carrier_freq_hz);
    const uint8_t corr_cmd = cfg.corrector_enabled ? 0x71 : 0x72;

    if (!write_byte(gain_cmd)) { close(); return DSP_ERR_PORT_OPEN; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!write_byte(freq_cmd)) { close(); return DSP_ERR_PORT_OPEN; }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!write_byte(corr_cmd)) { close(); return DSP_ERR_PORT_OPEN; }

    return DSP_OK;
}

void LegacySerialUsoSource::close() {
    if (handle_ >= 0) {
        /* Stop streaming command 0x10 (YCO8_1.cpp line 343) */
        write_byte(0x10);
        close_serial_handle(handle_);
        handle_ = -1;
    }
    synced_ = false;
}

bool LegacySerialUsoSource::is_open() const {
    return handle_ >= 0;
}

bool LegacySerialUsoSource::write_byte(uint8_t b) {
    return serial_write_byte(handle_, b);
}

int32_t LegacySerialUsoSource::read_exact(uint8_t* buf, int32_t len) {
    return serial_read_bytes(handle_, buf, len, 200);
}

bool LegacySerialUsoSource::resync_stream() {
    const int32_t payload_bytes = (bit_mode_ == 32) ? 4 : 3;
    uint8_t prev = 0;
    uint8_t cur  = 0;
    for (int tries = 0; tries < 32; ++tries) {
        if (read_exact(&cur, 1) != 1) return false;
        if ((prev == 0xFF || prev == 0xFE) && cur == 0x00) {
            uint8_t discard[8]{};
            /* Consume remaining bytes of this packet so the next packet starts cleanly at 0x00 */
            if (read_exact(discard, payload_bytes + 1) == payload_bytes + 1) {
                synced_ = true;
                return true;
            }
            return false;
        }
        prev = cur;
    }
    return false;
}

int32_t LegacySerialUsoSource::read_samples(
    double* out_samples,
    int32_t max_samples,
    int32_t* out_overflow
) {
    if (!is_open() || !out_samples || max_samples <= 0) return DSP_ERR_INVALID_ARG;
    if (out_overflow) *out_overflow = 0;

    if (!synced_ && !resync_stream()) {
        return 0;
    }

    const int32_t pkt_len = (bit_mode_ == 32) ? 6 : 5;
    int32_t count = 0;
    uint8_t buf[8]{};

    while (count < max_samples) {
        int32_t got = read_exact(buf, pkt_len);
        if (got == 0) break;
        if (got < pkt_len) {
            synced_ = false;
            break;
        }

        int32_t sample = 0;
        int32_t rc = parse_packet(buf, pkt_len, bit_mode_, &sample);
        if (rc == DSP_ERR_OVERFLOW) {
            if (out_overflow) *out_overflow = 1;
            out_samples[count++] = static_cast<double>(sample);
        } else if (rc == DSP_OK) {
            out_samples[count++] = static_cast<double>(sample);
        } else {
            synced_ = false;
            if (!resync_stream()) break;
        }
    }

    return count;
}

int32_t LegacySerialUsoSource::set_gain(int32_t gain_index) {
    gain_index_ = gain_index;
    if (!is_open()) return DSP_OK;
    return write_byte(legacy_gain_command_byte(gain_index_, bit_mode_)) ? DSP_OK : DSP_ERR_PORT_OPEN;
}

int32_t LegacySerialUsoSource::set_corrector(bool enabled) {
    if (!is_open()) return DSP_OK;
    return write_byte(enabled ? 0x71 : 0x72) ? DSP_OK : DSP_ERR_PORT_OPEN;
}

/* ============================================================================
 * 2. NewAdc32UsoSource Implementation (8 samples/half-period = 256 samples/vec)
 * ============================================================================ */
NewAdc32UsoSource::NewAdc32UsoSource()
    : handle_(-1), gain_index_(0), corrector_(true) {}

NewAdc32UsoSource::~NewAdc32UsoSource() {
    close();
}

int32_t NewAdc32UsoSource::open(const DspConfig& cfg) {
    close();
    gain_index_ = cfg.gain_index;
    corrector_  = cfg.corrector_enabled != 0;
    handle_ = open_serial_57600_8n1(cfg.port_or_path, 500);
    if (handle_ < 0) return DSP_ERR_PORT_OPEN;
    return DSP_OK;
}

void NewAdc32UsoSource::close() {
    if (handle_ >= 0) {
        close_serial_handle(handle_);
        handle_ = -1;
    }
}

bool NewAdc32UsoSource::is_open() const {
    return handle_ >= 0;
}

int32_t NewAdc32UsoSource::read_samples(
    double* out_samples,
    int32_t max_samples,
    int32_t* out_overflow
) {
    if (!is_open() || !out_samples || max_samples <= 0) return DSP_ERR_INVALID_ARG;
    if (out_overflow) *out_overflow = 0;

    int32_t count = 0;
    uint8_t raw[4]{};
    while (count < max_samples) {
        int32_t got = serial_read_bytes(handle_, raw, 4, 100);
        if (got < 4) break;
        uint32_t u =
            static_cast<uint32_t>(raw[0]) |
            (static_cast<uint32_t>(raw[1]) << 8) |
            (static_cast<uint32_t>(raw[2]) << 16) |
            (static_cast<uint32_t>(raw[3]) << 24);
        int32_t s = static_cast<int32_t>(u);
        if (s == INT32_MAX || s == INT32_MIN) {
            if (out_overflow) *out_overflow = 1;
        }
        out_samples[count++] = static_cast<double>(s);
    }
    return count;
}

int32_t NewAdc32UsoSource::set_gain(int32_t gain_index) {
    gain_index_ = gain_index;
    return DSP_OK;
}

int32_t NewAdc32UsoSource::set_corrector(bool enabled) {
    corrector_ = enabled;
    return DSP_OK;
}

/* ============================================================================
 * 3. SmpFilePlayerSource Implementation (Legacy sun32 binary .smp recordings)
 * ============================================================================ */
SmpFilePlayerSource::SmpFilePlayerSource()
    : file_(nullptr), bit_mode_(32), playback_speed_(1), sample_rate_hz_(250.0), samples_read_(0) {}

SmpFilePlayerSource::~SmpFilePlayerSource() {
    close();
}

int32_t SmpFilePlayerSource::open(const DspConfig& cfg) {
    close();
    if (cfg.port_or_path[0] == '\0') return DSP_ERR_INVALID_ARG;
    file_ = std::fopen(cfg.port_or_path, "rb");
    if (!file_) return DSP_ERR_FILE_OPEN;
    bit_mode_       = (cfg.uso_bit_mode == 24) ? 24 : 32;
    playback_speed_ = cfg.playback_speed;
    sample_rate_hz_ = (cfg.sample_rate_hz > 1.0) ? cfg.sample_rate_hz : 250.0;
    samples_read_   = 0;
    return DSP_OK;
}

void SmpFilePlayerSource::close() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

bool SmpFilePlayerSource::is_open() const {
    return file_ != nullptr;
}

void SmpFilePlayerSource::set_playback_speed(int32_t speed) {
    playback_speed_ = speed;
}

int32_t SmpFilePlayerSource::read_samples(
    double* out_samples,
    int32_t max_samples,
    int32_t* out_overflow
) {
    if (!file_ || !out_samples || max_samples <= 0) return DSP_ERR_INVALID_ARG;
    if (out_overflow) *out_overflow = 0;

    std::vector<int32_t> raw(static_cast<size_t>(max_samples));
    size_t got = std::fread(raw.data(), sizeof(int32_t), raw.size(), file_);
    if (got == 0) return 0; /* EOF */

    for (size_t i = 0; i < got; ++i) {
        int32_t v = raw[i];
        if (bit_mode_ == 24) {
            v = v >> 8; /* Matches YCO8_1.cpp line 134 */
        }
        out_samples[i] = static_cast<double>(v);
    }

    samples_read_ += got;

    /* Real-time pacing when playback_speed_ > 0 (0 = instant offline processing) */
    if (playback_speed_ > 0 && sample_rate_hz_ > 0.0) {
        double block_us = (static_cast<double>(got) * 1e6) / (sample_rate_hz_ * static_cast<double>(playback_speed_));
        if (block_us >= 200.0) {
            std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(block_us)));
        }
    }

    return static_cast<int32_t>(got);
}

int32_t SmpFilePlayerSource::set_gain(int32_t) { return DSP_OK; }
int32_t SmpFilePlayerSource::set_corrector(bool) { return DSP_OK; }

} // namespace dsp

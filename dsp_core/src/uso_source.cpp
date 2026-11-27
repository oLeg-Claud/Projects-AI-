#include "uso_source.hpp"
#include "dsp_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

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
    cto.WriteTotalTimeoutConstant   = 40;
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

void purge_serial_handle(intptr_t handle) {
    if (handle < 0) return;
#if defined(_WIN32)
    PurgeComm(reinterpret_cast<HANDLE>(handle),
              PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR);
#else
    tcflush(static_cast<int>(handle), TCIOFLUSH);
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

bool serial_write_bytes(intptr_t handle, const uint8_t* data, int32_t len) {
    if (handle < 0 || !data || len <= 0) return false;
#if defined(_WIN32)
    DWORD written = 0;
    return WriteFile(reinterpret_cast<HANDLE>(handle), data, static_cast<DWORD>(len), &written, nullptr)
           && static_cast<int32_t>(written) == len;
#else
    int32_t total = 0;
    while (total < len) {
        ssize_t w = ::write(static_cast<int>(handle), data + total, static_cast<size_t>(len - total));
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }
        if (w == 0) return false;
        total += static_cast<int32_t>(w);
    }
    return true;
#endif
}

bool serial_write_byte(intptr_t handle, uint8_t b) {
    return serial_write_bytes(handle, &b, 1);
}

bool serial_transmit_immediate(intptr_t handle, uint8_t b) {
    if (handle < 0) return false;
#if defined(_WIN32)
    if (TransmitCommChar(reinterpret_cast<HANDLE>(handle), static_cast<char>(b))) {
        return true;
    }
    return serial_write_byte(handle, b);
#else
    return serial_write_byte(handle, b);
#endif
}

FILE* open_file_utf8_rb(const char* path) {
    if (!path || path[0] == '\0') return nullptr;
#if defined(_WIN32)
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wlen > 0) {
        std::wstring wpath(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path, -1, &wpath[0], wlen);
        FILE* f = nullptr;
        if (_wfopen_s(&f, wpath.c_str(), L"rb") == 0 && f != nullptr) {
            return f;
        }
    }
#endif
    return std::fopen(path, "rb");
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

bool try_v2_hello_on_handle(intptr_t h, usov2::HelloRsp& out_hello) {
    uint8_t cmd[usov2::kMaxFrame]{};
    unsigned n = usov2::encode_cmd(usov2::CMD_HELLO, 1u, nullptr, 0u, cmd);
    if (!serial_write_bytes(h, cmd, static_cast<int32_t>(n))) return false;

    usov2::StreamParser parser;
    usov2::Frame frame;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(90);
    uint8_t buf[64]{};

    while (std::chrono::steady_clock::now() < deadline) {
        int32_t got = serial_read_bytes(h, buf, sizeof(buf), 40);
        if (got <= 0) break;
        for (int32_t i = 0; i < got; ++i) {
            if (parser.feed(buf[i], frame) == 1 && frame.msg_type == usov2::RSP_HELLO) {
                if (usov2::unpack_hello_rsp(frame.payload, frame.payload_len, out_hello)) {
                    return true;
                }
            }
        }
    }
    return false;
}

int32_t probe_port_internal(const char* port_name, int32_t* out_uso_id, std::string* diag_out) {
    if (!port_name || !out_uso_id) return DSP_ERR_INVALID_ARG;
    *out_uso_id = 0;

    intptr_t h = open_serial_57600_8n1(port_name, 70);
    if (h < 0) {
        if (diag_out) *diag_out = std::string(port_name) + "(open_err)";
        return DSP_ERR_PORT_OPEN;
    }

    auto try_legacy = [&](uint8_t* last_buf, int32_t* last_got) -> bool {
        serial_transmit_immediate(h, 0xAA);
        uint8_t ack = 0;
        (void)serial_read_bytes(h, &ack, 1, 50);

        if (!serial_write_byte(h, 0x66)) {
            *last_got = -1;
            return false;
        }
        uint8_t info[5]{};
        int32_t got = serial_read_bytes(h, info, 5, 100);
        *last_got = got;
        if (got > 0) std::memcpy(last_buf, info, static_cast<size_t>(got));

        if (got == 5 && info[0] == 0x01 && info[4] == 0xFF) {
            *out_uso_id = static_cast<int32_t>(info[1]);
            return true;
        }
        return false;
    };

    /* 1. Try USO-NG Protocol V2 first (0xA5 0x5A CMD_HELLO — ignored harmlessly by legacy USO) */
    usov2::HelloRsp hello{};
    bool is_v2 = try_v2_hello_on_handle(h, hello);
    bool ok = false;
    uint8_t rx[5]{};
    int32_t got = 0;

    if (is_v2) {
        *out_uso_id = static_cast<int32_t>(hello.device_id);
        ok = true;
    } else {
        /* 2. Try Legacy handshake (0xAA -> 0x66) */
        ok = try_legacy(rx, &got);

        /* 3. Pass 2 fallback: stop active stream (0x10 stops BOTH legacy and V2 streams!) */
        if (!ok) {
            serial_write_byte(h, 0x10);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            purge_serial_handle(h);
            is_v2 = try_v2_hello_on_handle(h, hello);
            if (is_v2) {
                *out_uso_id = static_cast<int32_t>(hello.device_id);
                ok = true;
            } else {
                ok = try_legacy(rx, &got);
            }
        }
    }

    close_serial_handle(h);

    if (diag_out) {
        if (ok && is_v2) {
            *diag_out = std::string(port_name) + "(УСО#" + std::to_string(*out_uso_id) +
                        " V2 v" + std::to_string(hello.fw_major) + "." + std::to_string(hello.fw_minor) + ")";
        } else if (ok) {
            *diag_out = std::string(port_name) + "(УСО#" + std::to_string(*out_uso_id) + " Legacy)";
        } else if (got <= 0) {
            *diag_out = std::string(port_name) + "(0б)";
        } else {
            char hex[32]{};
            int pos = 0;
            for (int32_t i = 0; i < got && i < 5; ++i) {
                pos += std::snprintf(hex + pos, sizeof(hex) - static_cast<size_t>(pos),
                                     (i == 0 ? "%02X" : " %02X"), rx[i]);
            }
            *diag_out = std::string(port_name) + "(rx:" + hex + ")";
        }
    }

    return ok ? DSP_OK : DSP_ERR_NOT_FOUND;
}

std::vector<std::string> enumerate_serial_ports() {
    std::vector<std::string> ports;
    auto add_unique = [&](const std::string& p) {
        if (p.empty()) return;
        for (const auto& existing : ports) {
            if (existing == p) return;
        }
        ports.push_back(p);
    };

#if defined(_WIN32)
    std::vector<char> dos_buf(65536, '\0');
    DWORD len = QueryDosDeviceA(nullptr, dos_buf.data(), static_cast<DWORD>(dos_buf.size()));
    if (len > 0) {
        const char* p = dos_buf.data();
        while (*p != '\0') {
            std::string name(p);
            if (name.size() >= 4 &&
                (name[0] == 'C' || name[0] == 'c') &&
                (name[1] == 'O' || name[1] == 'o') &&
                (name[2] == 'M' || name[2] == 'm')) {
                bool digits = true;
                for (size_t i = 3; i < name.size(); ++i) {
                    if (name[i] < '0' || name[i] > '9') { digits = false; break; }
                }
                if (digits) add_unique("COM" + name.substr(3));
            }
            p += name.size() + 1;
        }
    }

    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char val_name[256]{};
        char val_data[256]{};
        for (DWORD idx = 0; ; ++idx) {
            DWORD name_len = static_cast<DWORD>(sizeof(val_name));
            DWORD data_len = static_cast<DWORD>(sizeof(val_data) - 1);
            DWORD val_type = 0;
            LONG rc = RegEnumValueA(
                hKey, idx, val_name, &name_len, nullptr, &val_type,
                reinterpret_cast<LPBYTE>(val_data), &data_len
            );
            if (rc == ERROR_NO_MORE_ITEMS) break;
            if (rc == ERROR_SUCCESS && val_type == REG_SZ && data_len > 0) {
                val_data[data_len] = '\0';
                add_unique(val_data);
            }
        }
        RegCloseKey(hKey);
    }

    std::sort(ports.begin(), ports.end(), [](const std::string& a, const std::string& b) {
        auto parse_com = [](const std::string& s) -> int {
            if (s.size() > 3 && (s[0] == 'C' || s[0] == 'c') &&
                (s[1] == 'O' || s[1] == 'o') && (s[2] == 'M' || s[2] == 'm')) {
                return std::atoi(s.c_str() + 3);
            }
            return 9999;
        };
        int na = parse_com(a);
        int nb = parse_com(b);
        return (na != nb) ? (na < nb) : (a < b);
    });
#else
    const char* prefixes[] = { "/dev/ttyUSB", "/dev/ttyACM" };
    for (const char* pfx : prefixes) {
        for (int i = 0; i < 16; ++i) {
            std::string candidate = std::string(pfx) + std::to_string(i);
            if (::access(candidate.c_str(), R_OK | W_OK) == 0) {
                add_unique(candidate);
            }
        }
    }
#endif

    return ports;
}

} // namespace

/* ============================================================================
 * 1. Dual-Mode LegacySerialUsoSource (Auto V2 -> Legacy Fallback)
 * ============================================================================ */
LegacySerialUsoSource::LegacySerialUsoSource()
    : handle_(-1),
      bit_mode_(32),
      gain_index_(0),
      synced_(false),
      v2_active_(false),
      v2_seq_(1u)
{
    dsp_uso_v2_options_default(&v2_opts_);
    std::memset(&runtime_info_, 0, sizeof(runtime_info_));
}

LegacySerialUsoSource::~LegacySerialUsoSource() {
    close();
}

int32_t LegacySerialUsoSource::probe_port(const char* port_name, int32_t* out_uso_id) {
    return probe_port_internal(port_name, out_uso_id, nullptr);
}

int32_t LegacySerialUsoSource::scan_ports(char* out_port_buf, int32_t buf_len, int32_t* out_uso_id) {
    return scan_ports_diag(out_port_buf, buf_len, out_uso_id, nullptr, 0);
}

int32_t LegacySerialUsoSource::scan_ports_diag(
    char* out_port_buf,
    int32_t buf_len,
    int32_t* out_uso_id,
    char* out_diag_buf,
    int32_t diag_len
) {
    if (!out_port_buf || buf_len <= 1 || !out_uso_id) return DSP_ERR_INVALID_ARG;
    out_port_buf[0] = '\0';
    *out_uso_id = 0;
    if (out_diag_buf && diag_len > 0) out_diag_buf[0] = '\0';

    std::vector<std::string> ports = enumerate_serial_ports();
    if (ports.empty()) {
        if (out_diag_buf && diag_len > 1) {
            std::snprintf(out_diag_buf, static_cast<size_t>(diag_len), "COM-порты не найдены");
        }
        return DSP_ERR_NOT_FOUND;
    }

    std::string diag;
    for (const auto& port : ports) {
        int32_t id = 0;
        std::string item_diag;
        int32_t rc = probe_port_internal(port.c_str(), &id, &item_diag);
        if (!diag.empty()) diag += ", ";
        diag += item_diag;

        if (rc == DSP_OK) {
            std::snprintf(out_port_buf, static_cast<size_t>(buf_len), "%s", port.c_str());
            *out_uso_id = id;
            if (out_diag_buf && diag_len > 1) {
                std::snprintf(out_diag_buf, static_cast<size_t>(diag_len), "%s", diag.c_str());
            }
            return DSP_OK;
        }
    }

    if (out_diag_buf && diag_len > 1) {
        std::snprintf(out_diag_buf, static_cast<size_t>(diag_len), "%s", diag.c_str());
    }
    return DSP_ERR_NOT_FOUND;
}

bool LegacySerialUsoSource::write_byte(uint8_t b) {
    return serial_write_byte(handle_, b);
}

bool LegacySerialUsoSource::write_bytes(const uint8_t* data, int32_t len) {
    return serial_write_bytes(handle_, data, len);
}

int32_t LegacySerialUsoSource::read_exact(uint8_t* buf, int32_t len) {
    return serial_read_bytes(handle_, buf, len, 25);
}

bool LegacySerialUsoSource::send_v2_cmd(
    uint8_t msg_type,
    const uint8_t* payload,
    uint8_t payload_len,
    uint16_t* out_seq
) {
    uint16_t seq = v2_seq_++;
    if (out_seq) *out_seq = seq;
    uint8_t wire[usov2::kMaxFrame]{};
    unsigned n = usov2::encode_cmd(msg_type, seq, payload, payload_len, wire);
    if (n == 0u) return false;
    return write_bytes(wire, static_cast<int32_t>(n));
}

bool LegacySerialUsoSource::wait_v2_response(
    uint8_t expected_msg_type, uint16_t expected_seq,
    usov2::Frame& out_frame, int32_t timeout_ms
) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    uint8_t buf[128]{};
    do {
        if (v2_receiver_.take_response(expected_msg_type, expected_seq, out_frame)) return true;
        const int32_t got = serial_read_bytes(handle_, buf, sizeof(buf), 50);
        if (got > 0) v2_receiver_.feed(buf, static_cast<size_t>(got));
    } while (std::chrono::steady_clock::now() < deadline);
    return v2_receiver_.take_response(expected_msg_type, expected_seq, out_frame);
}

bool LegacySerialUsoSource::start_v2_stream(const DspConfig& cfg) {
    // Finish the previous session before discarding its queued samples.
    uint16_t stop_seq = 0;
    usov2::Frame stopped{};
    if (!send_v2_cmd(usov2::CMD_STOP, nullptr, 0, &stop_seq) ||
        !wait_v2_response(usov2::RSP_ACK, stop_seq, stopped, 500) ||
        stopped.payload_len != 2 || stopped.payload[0] != usov2::CMD_STOP ||
        stopped.payload[1] != usov2::ST_OK) return false;
    v2_receiver_.reset();
    /* Assemble V2 configuration combining DspConfig + DspUsoV2Options */
    usov2::WireConfig wcfg{};
    double f_mhz = std::round(std::max(0.5, cfg.carrier_freq_hz) * 1000.0);
    wcfg.carrier_freq_mhz   = static_cast<uint16_t>(std::min(65000.0, f_mhz));
    wcfg.dr_code            = (v2_opts_.adaptive_rate != 0)
                                  ? static_cast<uint8_t>(usov2::DR_AUTO)
                                  : static_cast<uint8_t>(std::clamp(v2_opts_.dr_code, 0x04, 0x0B));
    wcfg.decimation         = static_cast<uint8_t>(std::clamp(v2_opts_.decimation, 0, 16));
    wcfg.pga_gain           = static_cast<uint8_t>(std::clamp(cfg.gain_index, 0, 5));
    wcfg.adc_filter         = static_cast<uint8_t>(std::clamp(v2_opts_.adc_filter, 0, 4));
    wcfg.adc_flags          = static_cast<uint8_t>(
                                  (v2_opts_.chop_enabled ? usov2::kAdcFlagChop : 0u) |
                                  (v2_opts_.vbias_enabled ? usov2::kAdcFlagVbias : 0u)
                              );
    wcfg.inpmux             = static_cast<uint8_t>(v2_opts_.inpmux ? v2_opts_.inpmux : 0x89);
    wcfg.dsp_lpf_mode       = static_cast<uint8_t>(std::clamp(v2_opts_.mcu_lpf_mode, 0, 2));
    wcfg.batch_size         = static_cast<uint8_t>(std::clamp(v2_opts_.batch_size, 0, static_cast<int>(usov2::kMaxBatchSamples)));
    wcfg.dsp_lpf_cutoff_dhz = (v2_opts_.mcu_lpf_cutoff_hz > 0.0)
                                  ? static_cast<uint16_t>(std::min(65000.0, std::round(v2_opts_.mcu_lpf_cutoff_hz * 10.0)))
                                  : 0u;
    double hpf_hz           = cfg.corrector_enabled
                                  ? (v2_opts_.mcu_hpf_cutoff_hz > 0.0 ? v2_opts_.mcu_hpf_cutoff_hz : 0.1)
                                  : 0.0;
    wcfg.dsp_hpf_cutoff_mhz = static_cast<uint16_t>(std::min(65000.0, std::round(hpf_hz * 1000.0)));
    wcfg.stream_flags       = v2_opts_.auto_recover ? usov2::kStreamAutoRecover : 0u;

    uint8_t cfg_wire[usov2::kCfgWireLen]{};
    usov2::pack_config(wcfg, cfg_wire);

    uint16_t start_seq = 0;
    if (!send_v2_cmd(usov2::CMD_START, cfg_wire, sizeof(cfg_wire), &start_seq)) {
        return false;
    }

    usov2::Frame sf{};
    if (!wait_v2_response(usov2::RSP_START, start_seq, sf, 300)) {
        return false;
    }

    usov2::StartRsp srsp{};
    if (!usov2::unpack_start_rsp(sf.payload, sf.payload_len, srsp) || srsp.status != usov2::ST_OK) {
        runtime_info_.hw_fault = (srsp.status == usov2::ST_ERR_ADC_FAULT) ? 1 : 0;
        return false;
    }

    runtime_info_.dr_code               = static_cast<int32_t>(srsp.dr_code);
    runtime_info_.decimation            = static_cast<int32_t>(srsp.decimation);
    runtime_info_.adc_filter            = static_cast<int32_t>(srsp.adc_filter);
    runtime_info_.chop_enabled          = (srsp.adc_flags & usov2::kAdcFlagChop) ? 1 : 0;
    runtime_info_.inpmux                = static_cast<int32_t>(srsp.inpmux);
    runtime_info_.active_sample_rate_hz = static_cast<double>(srsp.effective_fs_dhz) * 0.1;
    runtime_info_.hw_sample_rate_hz     = static_cast<double>(srsp.hw_fs_dhz) * 0.1;

    v2_active_              = true;
    return true;
}

bool LegacySerialUsoSource::try_open_v2(const DspConfig& cfg) {
    v2_receiver_.reset();
    usov2::HelloRsp hello{};

    auto do_hello = [&]() -> bool {
        uint16_t seq = 0;
        if (!send_v2_cmd(usov2::CMD_HELLO, nullptr, 0u, &seq)) return false;
        usov2::Frame f{};
        if (!wait_v2_response(usov2::RSP_HELLO, seq, f, 100)) return false;
        return usov2::unpack_hello_rsp(f.payload, f.payload_len, hello);
    };

    if (!do_hello()) {
        /* If device was mid-stream, stop it cleanly and retry HELLO once */
        (void)send_v2_cmd(usov2::CMD_STOP, nullptr, 0u);
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        purge_serial_handle(handle_);
        v2_receiver_.reset();
        if (!do_hello()) {
            return false;
        }
    }

    runtime_info_.protocol_version = 2;
    runtime_info_.uso_id           = static_cast<int32_t>(hello.device_id);
    runtime_info_.fw_major         = static_cast<int32_t>(hello.fw_major);
    runtime_info_.fw_minor         = static_cast<int32_t>(hello.fw_minor);
    runtime_info_.fw_patch         = static_cast<int32_t>(hello.fw_patch);
    runtime_info_.adc_id           = static_cast<int32_t>(hello.adc_id);
    runtime_info_.adc_clock_hz     = hello.adc_clock_hz;
    runtime_info_.hw_fault         = static_cast<int32_t>(hello.fault);

    /* If ADC had a latched fault, attempt one controlled soft-reset via V2! */
    if (!hello.ready || hello.fault) {
        uint16_t rseq = 0;
        if (send_v2_cmd(usov2::CMD_RESET_ADC, nullptr, 0u, &rseq)) {
            usov2::Frame rf{};
            if (wait_v2_response(usov2::RSP_ACK, rseq, rf, 2500) &&
                rf.payload_len >= 2u && rf.payload[1] == usov2::ST_OK) {
                runtime_info_.hw_fault = 0;
            }
        }
    }

    return start_v2_stream(cfg);
}

int32_t LegacySerialUsoSource::open(const DspConfig& cfg) {
    close();
    bit_mode_   = (cfg.uso_bit_mode == 24) ? 24 : 32;
    gain_index_ = cfg.gain_index;
    synced_     = false;
    v2_active_  = false;
    std::memset(&runtime_info_, 0, sizeof(runtime_info_));

    handle_ = open_serial_57600_8n1(cfg.port_or_path, 40);
    if (handle_ < 0) return DSP_ERR_PORT_OPEN;

    purge_serial_handle(handle_);

    /* Auto-negotiate USO-NG Protocol V2 first; seamlessly fall back to Legacy uco32 if not supported */
    if (try_open_v2(cfg)) {
        return DSP_OK;
    }

    if (runtime_info_.protocol_version == 2) {
        // A recognized V2 device rejecting configuration is not a legacy device.
        close();
        return DSP_ERR_INVALID_ARG;
    }

    /* Legacy fallback initialization sequence (YCO8_1.cpp lines 67-99, uco32) */
    purge_serial_handle(handle_);
    // The v2.2 prototype speaks legacy framing but samples at 400 Hz.
    // Only use that rate when its explicit INFO signature confirms the firmware.
    double legacy_fs = cfg.sample_rate_hz > 1.0 ? cfg.sample_rate_hz : 250.0;
    if (write_byte(0xA1)) {
        uint8_t info[128]{};
        const int got = serial_read_bytes(handle_, info, sizeof(info), 150);
        const std::string text(reinterpret_cast<const char*>(info), got > 0 ? got : 0);
        if (text.find("USO32_App_v2") != std::string::npos && text.find("400Hz") != std::string::npos)
            legacy_fs = 400.0;
    }
    purge_serial_handle(handle_);
    const uint8_t gain_cmd = legacy_gain_command_byte(cfg.gain_index, bit_mode_);
    const uint8_t freq_cmd = legacy_freq_command_byte(cfg.carrier_freq_hz);
    legacy_freq_byte_ = freq_cmd;
    const uint8_t corr_cmd = cfg.corrector_enabled ? 0x71 : 0x72;

    if (!write_byte(gain_cmd)) { close(); return DSP_ERR_PORT_OPEN; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!write_byte(freq_cmd)) { close(); return DSP_ERR_PORT_OPEN; }
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    if (!write_byte(corr_cmd)) { close(); return DSP_ERR_PORT_OPEN; }

    runtime_info_.protocol_version      = 1;
    runtime_info_.active_sample_rate_hz = legacy_fs;
    runtime_info_.hw_sample_rate_hz     = legacy_fs;
    return DSP_OK;
}

int32_t LegacySerialUsoSource::reconfigure(const DspConfig& cfg) {
    if (!is_open()) return DSP_ERR_PORT_OPEN;
    bit_mode_   = (cfg.uso_bit_mode == 24) ? 24 : 32;
    gain_index_ = std::clamp(cfg.gain_index, 0, 5);

    if (v2_active_) {
        return start_v2_stream(cfg) ? DSP_OK : DSP_ERR_PORT_OPEN;
    }

    /* Fast Legacy frequency/gain/corr restart on the already-open handle (~130 ms, zero COM rescan) */
    if (!write_byte(0x10)) return DSP_ERR_PORT_OPEN;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    purge_serial_handle(handle_);
    synced_ = false;

    const uint8_t gain_cmd = legacy_gain_command_byte(cfg.gain_index, bit_mode_);
    const uint8_t freq_cmd = legacy_freq_command_byte(cfg.carrier_freq_hz);
    legacy_freq_byte_ = freq_cmd;
    const uint8_t corr_cmd = cfg.corrector_enabled ? 0x71 : 0x72;

    if (!write_byte(gain_cmd)) return DSP_ERR_PORT_OPEN;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!write_byte(freq_cmd)) return DSP_ERR_PORT_OPEN;
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    if (!write_byte(corr_cmd)) return DSP_ERR_PORT_OPEN;
    return DSP_OK;
}

void LegacySerialUsoSource::close() {
    if (handle_ >= 0) {
        if (v2_active_) {
            (void)send_v2_cmd(usov2::CMD_STOP, nullptr, 0u);
        } else {
            (void)write_byte(0x10);
        }
        close_serial_handle(handle_);
        handle_ = -1;
    }
    synced_ = false;
    v2_active_ = false;
    v2_receiver_.reset();
}

bool LegacySerialUsoSource::is_open() const {
    return handle_ >= 0;
}

double LegacySerialUsoSource::effective_sample_rate_hz() const {
    return runtime_info_.active_sample_rate_hz;
}

void LegacySerialUsoSource::set_v2_options(const DspUsoV2Options& opts) {
    v2_opts_ = opts;
}

void LegacySerialUsoSource::get_runtime_info(DspUsoRuntimeInfo& out) const {
    out = runtime_info_;
    out.crc_errors = runtime_info_.crc_errors + v2_receiver_.crc_errors();
    if (v2_active_) out.rx_packets = v2_receiver_.packets();
    out.dropped_samples = std::max(out.dropped_samples, v2_receiver_.dropped_samples());
}

int32_t LegacySerialUsoSource::recalibrate() {
    if (!is_open() || !v2_active_) return DSP_ERR_NOT_FOUND;
    uint16_t seq = 0;
    if (!send_v2_cmd(usov2::CMD_RECALIBRATE, nullptr, 0u, &seq)) return DSP_ERR_PORT_OPEN;
    usov2::Frame f{};
    if (!wait_v2_response(usov2::RSP_CAL, seq, f, 4000)) return DSP_ERR_PORT_OPEN;
    return (f.payload_len >= 1u && f.payload[0] == usov2::ST_OK) ? DSP_OK : DSP_ERR_INVALID_ARG;
}

int32_t LegacySerialUsoSource::reset_adc() {
    if (!is_open() || !v2_active_) return DSP_ERR_NOT_FOUND;
    uint16_t seq = 0;
    if (!send_v2_cmd(usov2::CMD_RESET_ADC, nullptr, 0u, &seq)) return DSP_ERR_PORT_OPEN;
    usov2::Frame f{};
    if (!wait_v2_response(usov2::RSP_ACK, seq, f, 3000)) return DSP_ERR_PORT_OPEN;
    if (f.payload_len >= 2u && f.payload[1] == usov2::ST_OK) {
        runtime_info_.hw_fault = 0;
        return DSP_OK;
    }
    return DSP_ERR_INVALID_ARG;
}

int32_t LegacySerialUsoSource::refresh_diagnostics() {
    if (!is_open() || !v2_active_) return DSP_ERR_NOT_FOUND;
    uint16_t seq = 0;
    if (!send_v2_cmd(usov2::CMD_GET_DIAG, nullptr, 0u, &seq)) return DSP_ERR_PORT_OPEN;
    usov2::Frame f{};
    if (!wait_v2_response(usov2::RSP_DIAG, seq, f, 200)) return DSP_ERR_PORT_OPEN;
    usov2::DiagRsp d{};
    if (!usov2::unpack_diag_rsp(f.payload, f.payload_len, d)) return DSP_ERR_INVALID_ARG;
    runtime_info_.missed_drdy    = d.missed_drdy;
    runtime_info_.tx_overflows   = d.tx_overflows;
    runtime_info_.crc_errors     = d.v2_crc_errors;
    runtime_info_.adc_status     = static_cast<int32_t>(d.adc_status);
    runtime_info_.hw_fault       = (d.state_flags & 4u) ? 1 : 0;
    if (d.dropped_samples > runtime_info_.dropped_samples) {
        runtime_info_.dropped_samples = d.dropped_samples;
    }
    return DSP_OK;
}

bool LegacySerialUsoSource::resync_stream() {
    const int32_t payload_bytes = (bit_mode_ == 32) ? 4 : 3;
    uint8_t prev = 0;
    uint8_t cur  = 0;
    for (int tries = 0; tries < 48; ++tries) {
        if (read_exact(&cur, 1) != 1) return false;
        if ((prev == 0xFF || prev == 0xFE) && cur == 0x00) {
            uint8_t discard[8]{};
            if (read_exact(discard, payload_bytes + 1) == payload_bytes + 1) {
                const uint8_t trailer = discard[payload_bytes];
                if (trailer == 0xFF || trailer == 0xFE) {
                    synced_ = true;
                    return true;
                }
                prev = trailer;
                continue;
            }
            return false;
        }
        prev = cur;
    }
    return false;
}

int32_t LegacySerialUsoSource::read_samples_v2(
    double* out_samples, int32_t max_samples, int32_t* out_overflow
) {
    auto drain = [&]() {
        return v2_receiver_.read(out_samples, max_samples, out_overflow,
                                 &runtime_info_.active_sample_rate_hz);
    };
    int count = drain();
    if (count > 0) return count;
    uint8_t raw[128]{};
    const int32_t got = serial_read_bytes(handle_, raw, sizeof(raw), 60);
    if (got > 0) v2_receiver_.feed(raw, static_cast<size_t>(got));
    return drain();
}

int32_t LegacySerialUsoSource::read_samples(
    double* out_samples,
    int32_t max_samples,
    int32_t* out_overflow
) {
    if (!is_open() || !out_samples || max_samples <= 0) return DSP_ERR_INVALID_ARG;
    if (out_overflow) *out_overflow = 0;

    if (v2_active_) {
        return read_samples_v2(out_samples, max_samples, out_overflow);
    }

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
            runtime_info_.rx_packets++;
        } else if (rc == DSP_OK) {
            out_samples[count++] = static_cast<double>(sample);
            runtime_info_.rx_packets++;
        } else {
            synced_ = false;
            if (!resync_stream()) break;
        }
    }

    return count;
}

int32_t LegacySerialUsoSource::set_gain(int32_t gain_index) {
    gain_index_ = std::clamp(gain_index, 0, 5);
    if (!is_open()) return DSP_OK;
    if (v2_active_) {
        uint8_t live[usov2::kLiveWireLen]{};
        usov2::pack_live(usov2::kLiveMaskGain, static_cast<uint8_t>(gain_index_), 0u, 0u, 0u, 0u, live);
        uint16_t seq = 0;
        usov2::Frame reply{};
        if (!send_v2_cmd(usov2::CMD_SET_LIVE, live, sizeof(live), &seq) ||
            !wait_v2_response(usov2::RSP_ACK, seq, reply, 500)) return DSP_ERR_PORT_OPEN;
        return reply.payload_len == 2 && reply.payload[0] == usov2::CMD_SET_LIVE &&
               reply.payload[1] == usov2::ST_OK ? DSP_OK : DSP_ERR_INVALID_ARG;
    }
    // Legacy START/gain is a two-byte command; retain the active filter byte.
    const uint8_t command[] = {legacy_gain_command_byte(gain_index_, bit_mode_), legacy_freq_byte_};
    return write_bytes(command, sizeof(command)) ? DSP_OK : DSP_ERR_PORT_OPEN;
}

int32_t LegacySerialUsoSource::set_corrector(bool enabled) {
    if (!is_open()) return DSP_OK;
    if (v2_active_) {
        double hpf_hz = enabled ? (v2_opts_.mcu_hpf_cutoff_hz > 0.0 ? v2_opts_.mcu_hpf_cutoff_hz : 0.1) : 0.0;
        uint16_t hpf_mhz = static_cast<uint16_t>(std::min(65000.0, std::round(hpf_hz * 1000.0)));
        uint8_t live[usov2::kLiveWireLen]{};
        usov2::pack_live(usov2::kLiveMaskHpf, 0u, 0u, 0u, 0u, hpf_mhz, live);
        uint16_t seq = 0;
        usov2::Frame reply{};
        if (!send_v2_cmd(usov2::CMD_SET_LIVE, live, sizeof(live), &seq) ||
            !wait_v2_response(usov2::RSP_ACK, seq, reply, 500)) return DSP_ERR_PORT_OPEN;
        return reply.payload_len == 2 && reply.payload[0] == usov2::CMD_SET_LIVE &&
               reply.payload[1] == usov2::ST_OK ? DSP_OK : DSP_ERR_INVALID_ARG;
    }
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
    file_ = open_file_utf8_rb(cfg.port_or_path);
    if (!file_) return DSP_ERR_FILE_OPEN;

    char magic[4]{};
    if (std::fread(magic, 1, 4, file_) == 4 && std::memcmp(magic, "----", 4) == 0) {
        if (std::fseek(file_, 2048, SEEK_SET) != 0) {
            std::fseek(file_, 0, SEEK_SET);
        }
    } else {
        std::fseek(file_, 0, SEEK_SET);
    }

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
    if (got == 0) return 0;

    for (size_t i = 0; i < got; ++i) {
        int32_t v = raw[i];
        if (bit_mode_ == 24) {
            v = v >> 8;
        }
        out_samples[i] = static_cast<double>(v);
    }

    samples_read_ += got;

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

#ifndef DSP_USO_SOURCE_HPP
#define DSP_USO_SOURCE_HPP

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <atomic>
#include "../include/dsp_core_api.h"

namespace dsp {

/**
 * Unified hardware/emulator input interface (TZ §3.1 — IUsoSource).
 */
class IUsoSource {
public:
    virtual ~IUsoSource() = default;

    virtual int32_t open(const DspConfig& cfg) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    /**
     * Read up to max_samples samples into out_samples (converted to double).
     * Sets *out_overflow = 1 if an ADC overflow marker (0xFE) was seen.
     * Returns number of samples read (>= 0), 0 on EOF/timeout, or < 0 on fatal error.
     */
    virtual int32_t read_samples(double* out_samples, int32_t max_samples, int32_t* out_overflow) = 0;

    /** Runtime control commands (gain / corrector). No-op for file player. */
    virtual int32_t set_gain(int32_t gain_index) = 0;
    virtual int32_t set_corrector(bool enabled) = 0;
};

/**
 * 1. Legacy Serial/USB USO (57600 8N1, 5-byte 24-bit or 6-byte 32-bit uco32).
 *    Implements cross-platform Win32 HANDLE / POSIX termios serial I/O.
 */
class LegacySerialUsoSource final : public IUsoSource {
public:
    LegacySerialUsoSource();
    ~LegacySerialUsoSource() override;

    int32_t open(const DspConfig& cfg) override;
    void close() override;
    bool is_open() const override;
    int32_t read_samples(double* out_samples, int32_t max_samples, int32_t* out_overflow) override;
    int32_t set_gain(int32_t gain_index) override;
    int32_t set_corrector(bool enabled) override;

    static int32_t probe_port(const char* port_name, int32_t* out_uso_id);
    static int32_t scan_ports(char* out_port_buf, int32_t buf_len, int32_t* out_uso_id);
    static int32_t scan_ports_diag(
        char* out_port_buf,
        int32_t buf_len,
        int32_t* out_uso_id,
        char* out_diag_buf,
        int32_t diag_len
    );
    static int32_t parse_packet(const uint8_t* packet, int32_t len, int32_t bit_mode, int32_t* out_sample);

private:
    bool write_byte(uint8_t b);
    int32_t read_exact(uint8_t* buf, int32_t len);
    bool resync_stream();

    intptr_t handle_;
    int32_t  bit_mode_;
    int32_t  gain_index_;
    bool     synced_;
};

/**
 * 2. New 32-bit ADC USO (8 samples per half-period -> 256 samples/vector).
 *    Reads 4-byte little-endian signed 32-bit frames from serial/USB stream or ring buffer.
 */
class NewAdc32UsoSource final : public IUsoSource {
public:
    NewAdc32UsoSource();
    ~NewAdc32UsoSource() override;

    int32_t open(const DspConfig& cfg) override;
    void close() override;
    bool is_open() const override;
    int32_t read_samples(double* out_samples, int32_t max_samples, int32_t* out_overflow) override;
    int32_t set_gain(int32_t gain_index) override;
    int32_t set_corrector(bool enabled) override;

private:
    intptr_t handle_;
    int32_t  gain_index_;
    bool     corrector_;
};

/**
 * 3. Binary .smp (sun32) File Player Emulator (TZ §3.1.3 & YCO8_1.cpp).
 *    Reads 32-bit signed integers (sun32) from legacy .smp recordings with
 *    configurable playback speed (x1, x2, x4, x8, or 0 = instant offline batch).
 */
class SmpFilePlayerSource final : public IUsoSource {
public:
    SmpFilePlayerSource();
    ~SmpFilePlayerSource() override;

    int32_t open(const DspConfig& cfg) override;
    void close() override;
    bool is_open() const override;
    int32_t read_samples(double* out_samples, int32_t max_samples, int32_t* out_overflow) override;
    int32_t set_gain(int32_t gain_index) override;
    int32_t set_corrector(bool enabled) override;

    void set_playback_speed(int32_t speed);
    uint64_t samples_read() const { return samples_read_; }

private:
    FILE*    file_;
    int32_t  bit_mode_;
    int32_t  playback_speed_;
    double   sample_rate_hz_;
    uint64_t samples_read_;
};

} // namespace dsp

#endif /* DSP_USO_SOURCE_HPP */

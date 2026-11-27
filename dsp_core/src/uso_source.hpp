#ifndef DSP_USO_SOURCE_HPP
#define DSP_USO_SOURCE_HPP

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>
#include <atomic>
#include "../include/dsp_core_api.h"
#include "uso_proto_v2.hpp"

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
     * Sets *out_overflow = 1 if an ADC overflow marker (0xFE / V2 PGA_OVF) was seen.
     * Returns number of samples read (>= 0), 0 on EOF/timeout, or < 0 on fatal error.
     */
    virtual int32_t read_samples(double* out_samples, int32_t max_samples, int32_t* out_overflow) = 0;

    /** Runtime control commands (gain / corrector). No-op for file player. */
    virtual int32_t set_gain(int32_t gain_index) = 0;
    virtual int32_t set_corrector(bool enabled) = 0;

    /** Negotiated actual sample rate (<= 0 means use cfg.sample_rate_hz). */
    virtual double effective_sample_rate_hz() const { return 0.0; }

    /** V2 hardware/DSP extensions (no-op defaults for legacy/file sources). */
    virtual void set_v2_options(const DspUsoV2Options&) {}
    virtual void get_runtime_info(DspUsoRuntimeInfo& out) const {
        std::memset(&out, 0, sizeof(out));
    }
    virtual int32_t recalibrate() { return DSP_ERR_NOT_FOUND; }
    virtual int32_t reset_adc() { return DSP_ERR_NOT_FOUND; }
    virtual int32_t refresh_diagnostics() { return DSP_ERR_NOT_FOUND; }
};

/**
 * 1. Dual-Mode Serial/USB USO32 Source:
 *    - Automatically probes Protocol V2 (0xA5 0x5A CMD_HELLO, CRC-16, 61-byte batches,
 *      adaptive ADS1262 DR/filters/CHOP/INPMUX) first.
 *    - Seamlessly falls back to Legacy USO32 (57600 8N1, 5-byte 24-bit or 6-byte 32-bit uco32)
 *      when an older USO board is connected.
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
    double effective_sample_rate_hz() const override;
    void set_v2_options(const DspUsoV2Options& opts) override;
    void get_runtime_info(DspUsoRuntimeInfo& out) const override;
    int32_t recalibrate() override;
    int32_t reset_adc() override;
    int32_t refresh_diagnostics() override;

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
    bool write_bytes(const uint8_t* data, int32_t len);
    int32_t read_exact(uint8_t* buf, int32_t len);
    bool resync_stream();

    /* V2 helpers */
    bool send_v2_cmd(uint8_t msg_type, const uint8_t* payload, uint8_t payload_len, uint16_t* out_seq = nullptr);
    bool wait_v2_response(uint8_t expected_msg_type, uint16_t expected_seq, usov2::Frame& out_frame, int32_t timeout_ms);
    bool try_open_v2(const DspConfig& cfg);
    int32_t read_samples_v2(double* out_samples, int32_t max_samples, int32_t* out_overflow);

    intptr_t handle_;
    int32_t  bit_mode_;
    int32_t  gain_index_;
    bool     synced_;

    /* Dual-mode V2 state */
    bool               v2_active_;
    DspUsoV2Options    v2_opts_;
    DspUsoRuntimeInfo  runtime_info_;
    usov2::StreamParser v2_parser_;
    uint16_t           v2_seq_;
    bool               v2_have_sample_idx_;
    uint32_t           v2_expected_sample_idx_;
    std::deque<double> v2_sample_queue_;
    bool               v2_pending_overflow_;
};

/**
 * 2. New 32-bit ADC USO (8 samples per half-period -> 256 samples/vector).
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

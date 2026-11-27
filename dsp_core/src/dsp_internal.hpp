#ifndef DSP_INTERNAL_HPP
#define DSP_INTERNAL_HPP

#include <array>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include "../include/dsp_core_api.h"

namespace dsp {

/* Exact Reed-Muller RM(1,4) BPSK Manchester 32-half-period codebooks from TRAEKT2k.cpp */
static constexpr std::array<std::array<uint32_t, DSP_RM14_CODES>, 2> kGcodes = {{
    {{
        0x33339999u, 0x33cc9966u, 0x363c6369u, 0x36c36396u,
        0x39336c66u, 0x39cc6c99u, 0x3c3c9696u, 0x3cc39669u,
        0x6369363cu, 0x639636c3u, 0x6666ccccu, 0x6699cc33u,
        0x6969c3c3u, 0x6996c33cu, 0x6c663933u, 0x6c9939ccu
    }},
    {{
        0xa95a9a69u, 0x565a6569u, 0x59aa6a99u, 0xa6aa9599u,
        0x659656a5u, 0x9a96a9a5u, 0x9566a655u, 0x6a665955u,
        0x5556999au, 0x55a99965u, 0x5a599695u, 0x5aa6966au,
        0x6665aaa9u, 0x669aaa56u, 0x696aa5a6u, 0x6995a559u
    }}
}};

/* Exact 128-chip Sync Preamble (СП) from TRAEKT2k.cpp */
static constexpr std::array<std::array<uint32_t, 4>, 2> kGsp = {{
    {{ 0xa5995a55u, 0x955aaa66u, 0x6969a9a6u, 0x596a5669u }},
    {{ 0x9955a956u, 0x595a6966u, 0x99a9aa5au, 0x56665aa9u }}
}};

/**
 * Repeater pause vector count per frequency (TZ §3.3).
 */
inline int32_t get_pause_vectors_per_slot(double carrier_freq_hz) {
    if (carrier_freq_hz >= 15.0) return 4;
    if (carrier_freq_hz >= 9.0)  return 3;
    if (carrier_freq_hz >= 4.5)  return 2;
    return 1;
}

/**
 * Base vector count and pause slot count per legacy format*_2k.h headers.
 */
struct FormatGeometry {
    int32_t base_vectors;
    int32_t pause_slots;
    bool    has_gk;
    bool    has_ndm;
    bool    is_geomash;
};

inline FormatGeometry get_format_geometry(int32_t format_id) {
    switch (format_id) {
        case DSP_FMT_STD_GK:       return { 39,  0, true,  false, false };
        case DSP_FMT_NDM_GK_RETR:  return { 104, 7, true,  true,  false };
        case DSP_FMT_GK_RETR:      return { 43,  4, true,  false, false };
        case DSP_FMT_STD:          return { 33,  0, false, false, false };
        case DSP_FMT_STD_RETR:     return { 37,  4, false, false, false };
        case DSP_FMT_GEOMASH:      return { 34,  0, false, false, true  };
        case DSP_FMT_GEOMASH_RETR: return { 35,  1, false, false, true  };
        default:                   return { 39,  0, true,  false, false };
    }
}

/**
 * Compute legacy frequency register byte (0..5) sent to USO hardware (YCO8_1.cpp lines 79-85).
 */
inline uint8_t legacy_freq_command_byte(double carrier_freq_hz) {
    if (carrier_freq_hz >= 9.0)  return 0; /* 16 Hz / 10 Hz */
    if (carrier_freq_hz >= 6.0)  return 1; /* 7 Hz */
    if (carrier_freq_hz >= 4.3)  return 2; /* 5 Hz */
    if (carrier_freq_hz >= 3.0)  return 3; /* 3.6 Hz */
    if (carrier_freq_hz >= 2.1)  return 4; /* 2.5 Hz */
    return 5;                              /* 1.8 Hz */
}

/**
 * Compute legacy gain command byte (YCO8_1.cpp lines 70-77).
 */
inline uint8_t legacy_gain_command_byte(int32_t gain_index, int32_t bit_mode) {
    int32_t idx = gain_index;
    if (idx < 0) idx = 0;
    if (idx > 5) idx = 5;
    return static_cast<uint8_t>((bit_mode == 32 ? 0x90 : 0x20) + idx);
}

/**
 * Exact downhole crystal-divider frequency multiplier pw[7] from TRAEKT2k.cpp line 220.
 * Legacy downhole tools generate half-periods from a binary crystal divider:
 *   TKADR5 = 201.326592 s for 512 half-periods at 1.25 Hz -> 0.393216 s/half-period at pw=1.
 * For example, nominal "5 Hz" (pw=4) is actually 5.086263 Hz (+1.725%) and "16 Hz" (pw=12.8)
 * is 16.276042 Hz (+1.725%). Using nominal integer Hz causes >2 chips of phase slip across
 * the 128-chip Sync Preamble and prevents SP lock!
 */
inline double exact_downhole_pw(double carrier_freq_hz) {
    if (carrier_freq_hz >= 13.0) return 12.8;           /* 16 Hz  -> 16.27604 Hz */
    if (carrier_freq_hz >= 8.5)  return 8.0;            /* 10 Hz  -> 10.17253 Hz */
    if (carrier_freq_hz >= 6.0)  return 5.6568424949;   /* 7 Hz   ->  7.19305 Hz */
    if (carrier_freq_hz >= 4.2)  return 4.0;            /* 5 Hz   ->  5.08626 Hz */
    if (carrier_freq_hz >= 3.0)  return 2.828427124745; /* 3.6 Hz ->  3.59653 Hz */
    if (carrier_freq_hz >= 2.1)  return 2.0;            /* 2.5 Hz ->  2.54313 Hz */
    return 1.41421356237;                               /* 1.8 Hz ->  1.79827 Hz */
}

/**
 * Compute nominal resampling step H that maps input sample_rate_hz onto virtual grid
 * DLPOLr = 13 samples per half-period (bit) using exact TRAEKT2k.cpp timing.
 */
inline double compute_nominal_step_h(double sample_rate_hz, double carrier_freq_hz, int32_t source_type) {
    if (source_type == DSP_SRC_NEW_ADC32) {
        return 8.0 / static_cast<double>(DSP_DLPOL_R);
    }
    if (sample_rate_hz <= 1e-6 || carrier_freq_hz <= 1e-6) return 1.0;
    constexpr double kHalfPeriodAtPw1Sec = 201.326592 / 512.0; /* 0.393216 s */
    const double pw = exact_downhole_pw(carrier_freq_hz);
    return sample_rate_hz * (kHalfPeriodAtPw1Sec / (static_cast<double>(DSP_DLPOL_R) * pw));
}

/**
 * High-precision SIMD-accelerated double dot product, sum, and energy computation.
 * Uses AVX2 -> SSE2 -> strict double scalar fallback (prevents 32-bit ADC precision loss).
 */
double simd_dot_double(const double* a, const double* b, size_t len);
double simd_energy_double(const double* a, size_t len);
double simd_sum_double(const double* a, size_t len);
double simd_centered_energy_double(const double* a, size_t len, double mean);
const char* active_simd_isa_name();

/**
 * Legacy bit-level helpers ported verbatim from KORDAT2k.cpp.
 */
void pars_bits(uint8_t src, int8_t begist, int8_t endist, uint16_t* ptrnazn, int8_t begnazn);
uint32_t join_5bit_to_bytes(const uint8_t* in, uint32_t len, uint8_t* out);
uint32_t compute_crc32_ieee(const uint8_t* data, size_t len);
double scale_gk_log(uint8_t code, double kgk_zts);
double rotor_deg(double up, double down);
double resolve_az_deg(double hx, double hy, double hz, double rzu, double rotk);

} // namespace dsp

#endif /* DSP_INTERNAL_HPP */

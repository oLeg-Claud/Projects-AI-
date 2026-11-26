#ifndef DSP_CORRELATOR_HPP
#define DSP_CORRELATOR_HPP

#include <array>
#include <cstdint>
#include <vector>
#include "../include/dsp_core_api.h"

namespace dsp {

struct SyncSearchResult {
    bool    found;
    int32_t peak_index;      /* Offset into search window where Sync Preamble starts */
    int32_t phase_polarity;  /* +1 or -1 */
    int32_t reliability_pct; /* 0..100 */
    double  peak_corr;       /* Signed normalized correlation (-1.0 .. +1.0) */
};

struct VectorDecodeResult {
    uint8_t code_5bit;       /* 0..31 (bits 0..3 = RM(1,4) index, bit 4 = phase inversion) */
    int32_t reliability_pct; /* 0..100 */
    int32_t peak_offset_ig;  /* Fine timing adjustment in [-iG, +iG) */
    double  peak_corr;       /* Signed normalized correlation */
};

/**
 * Soft-Decision Double-Precision Correlator (TZ §3.2):
 * - Never collapses ADC samples to hard bits before correlation.
 * - Uses full 1664-sample Sync Preamble template & 16 orthogonal 416-sample RM(1,4) templates.
 * - All inner products use SIMD AVX2/SSE2 with double accumulators (no 32-bit ADC precision loss).
 * - Implements the exact 2-stage search from KORDAT2k.cpp lines 90-143:
 *     Stage 1: Correlate all 16 templates at nominal vector start (indl + iG) to pick winning basis vector max0.
 *     Stage 2: Slide winning template across [indl, indl + 2*iG) to find fine peak & extract 5th bit from sign.
 */
class SoftCorrelator {
public:
    SoftCorrelator();

    /** Switch codebook Ncod (0 or 1) and rebuild expanded double templates. */
    void set_codebook(int32_t ncod);
    int32_t codebook() const { return ncod_; }

    /**
     * Search for the 128-chip (1664-sample) Sync Preamble across `buffer[0 .. len-1]`.
     * Evaluates lags from 0 up to `len - DSP_SYNC_SAMPLES`.
     */
    SyncSearchResult search_sync_preamble(
        const double* buffer,
        size_t len,
        int32_t dcp_threshold
    ) const;

    /**
     * Decode one 5-bit RM(1,4) vector from a contiguous virtual-grid slice.
     * `window_ptr` must point to at least `DSP_VEC_SAMPLES + 2 * search_ig` valid double samples,
     * where the nominal vector start is at `window_ptr + search_ig`.
     */
    VectorDecodeResult decode_vector(
        const double* window_ptr,
        int32_t search_ig,
        int32_t frame_phase
    ) const;

    /** Helper for unit tests: access expanded sync & RM(1,4) templates. */
    const std::array<double, DSP_SYNC_SAMPLES>& sync_template() const { return sync_tpl_; }
    const std::array<double, DSP_VEC_SAMPLES>& vec_template(int32_t idx) const { return vec_tpls_[idx & 0x0F]; }

private:
    void rebuild_templates();

    int32_t ncod_;
    std::array<double, DSP_SYNC_SAMPLES> sync_tpl_;
    double sync_energy_;
    std::array<std::array<double, DSP_VEC_SAMPLES>, DSP_RM14_CODES> vec_tpls_;
    std::array<double, DSP_RM14_CODES> vec_energies_;
};

} // namespace dsp

#endif /* DSP_CORRELATOR_HPP */

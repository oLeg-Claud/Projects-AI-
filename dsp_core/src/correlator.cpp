#include "correlator.hpp"
#include "dsp_internal.hpp"

#include <cmath>
#include <algorithm>

#if defined(__AVX2__)
  #include <immintrin.h>
#elif defined(__SSE2__)
  #include <emmintrin.h>
#endif

namespace dsp {

/* ============================================================================
 * SIMD Double-Precision Inner Product & Energy (AVX2 -> SSE2 -> Scalar)
 * Ensures zero precision loss even on full-scale 32-bit ADC samples (~2.1e9).
 * ============================================================================ */
const char* active_simd_isa_name() {
#if defined(__AVX2__)
    return "AVX2";
#elif defined(__SSE2__)
    return "SSE2";
#else
    return "SCALAR";
#endif
}

double simd_dot_double(const double* a, const double* b, size_t len) {
    size_t i = 0;
    double sum = 0.0;

#if defined(__AVX2__)
    const size_t vec_end = len & ~static_cast<size_t>(7);
    __m256d acc0 = _mm256_setzero_pd();
    __m256d acc1 = _mm256_setzero_pd();
    for (; i < vec_end; i += 8) {
        __m256d va0 = _mm256_loadu_pd(a + i);
        __m256d vb0 = _mm256_loadu_pd(b + i);
        __m256d va1 = _mm256_loadu_pd(a + i + 4);
        __m256d vb1 = _mm256_loadu_pd(b + i + 4);
  #if defined(__FMA__)
        acc0 = _mm256_fmadd_pd(va0, vb0, acc0);
        acc1 = _mm256_fmadd_pd(va1, vb1, acc1);
  #else
        acc0 = _mm256_add_pd(acc0, _mm256_mul_pd(va0, vb0));
        acc1 = _mm256_add_pd(acc1, _mm256_mul_pd(va1, vb1));
  #endif
    }
    acc0 = _mm256_add_pd(acc0, acc1);
    alignas(32) double tmp[4];
    _mm256_store_pd(tmp, acc0);
    sum = (tmp[0] + tmp[1]) + (tmp[2] + tmp[3]);
#elif defined(__SSE2__)
    const size_t vec_end = len & ~static_cast<size_t>(3);
    __m128d acc0 = _mm_setzero_pd();
    __m128d acc1 = _mm_setzero_pd();
    for (; i < vec_end; i += 4) {
        __m128d va0 = _mm_loadu_pd(a + i);
        __m128d vb0 = _mm_loadu_pd(b + i);
        __m128d va1 = _mm_loadu_pd(a + i + 2);
        __m128d vb1 = _mm_loadu_pd(b + i + 2);
        acc0 = _mm_add_pd(acc0, _mm_mul_pd(va0, vb0));
        acc1 = _mm_add_pd(acc1, _mm_mul_pd(va1, vb1));
    }
    acc0 = _mm_add_pd(acc0, acc1);
    alignas(16) double tmp[2];
    _mm_store_pd(tmp, acc0);
    sum = tmp[0] + tmp[1];
#endif

    for (; i < len; ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

double simd_energy_double(const double* a, size_t len) {
    size_t i = 0;
    double sum = 0.0;

#if defined(__AVX2__)
    const size_t vec_end = len & ~static_cast<size_t>(7);
    __m256d acc0 = _mm256_setzero_pd();
    __m256d acc1 = _mm256_setzero_pd();
    for (; i < vec_end; i += 8) {
        __m256d v0 = _mm256_loadu_pd(a + i);
        __m256d v1 = _mm256_loadu_pd(a + i + 4);
  #if defined(__FMA__)
        acc0 = _mm256_fmadd_pd(v0, v0, acc0);
        acc1 = _mm256_fmadd_pd(v1, v1, acc1);
  #else
        acc0 = _mm256_add_pd(acc0, _mm256_mul_pd(v0, v0));
        acc1 = _mm256_add_pd(acc1, _mm256_mul_pd(v1, v1));
  #endif
    }
    acc0 = _mm256_add_pd(acc0, acc1);
    alignas(32) double tmp[4];
    _mm256_store_pd(tmp, acc0);
    sum = (tmp[0] + tmp[1]) + (tmp[2] + tmp[3]);
#elif defined(__SSE2__)
    const size_t vec_end = len & ~static_cast<size_t>(3);
    __m128d acc0 = _mm_setzero_pd();
    __m128d acc1 = _mm_setzero_pd();
    for (; i < vec_end; i += 4) {
        __m128d v0 = _mm_loadu_pd(a + i);
        __m128d v1 = _mm_loadu_pd(a + i + 2);
        acc0 = _mm_add_pd(acc0, _mm_mul_pd(v0, v0));
        acc1 = _mm_add_pd(acc1, _mm_mul_pd(v1, v1));
    }
    acc0 = _mm_add_pd(acc0, acc1);
    alignas(16) double tmp[2];
    _mm_store_pd(tmp, acc0);
    sum = tmp[0] + tmp[1];
#endif

    for (; i < len; ++i) {
        sum += a[i] * a[i];
    }
    return sum;
}

double simd_sum_double(const double* a, size_t len) {
    size_t i = 0;
    double sum = 0.0;

#if defined(__AVX2__)
    const size_t vec_end = len & ~static_cast<size_t>(7);
    __m256d acc0 = _mm256_setzero_pd();
    __m256d acc1 = _mm256_setzero_pd();
    for (; i < vec_end; i += 8) {
        acc0 = _mm256_add_pd(acc0, _mm256_loadu_pd(a + i));
        acc1 = _mm256_add_pd(acc1, _mm256_loadu_pd(a + i + 4));
    }
    acc0 = _mm256_add_pd(acc0, acc1);
    alignas(32) double tmp[4];
    _mm256_store_pd(tmp, acc0);
    sum = (tmp[0] + tmp[1]) + (tmp[2] + tmp[3]);
#elif defined(__SSE2__)
    const size_t vec_end = len & ~static_cast<size_t>(3);
    __m128d acc0 = _mm_setzero_pd();
    __m128d acc1 = _mm_setzero_pd();
    for (; i < vec_end; i += 4) {
        acc0 = _mm_add_pd(acc0, _mm_loadu_pd(a + i));
        acc1 = _mm_add_pd(acc1, _mm_loadu_pd(a + i + 2));
    }
    acc0 = _mm_add_pd(acc0, acc1);
    alignas(16) double tmp[2];
    _mm_store_pd(tmp, acc0);
    sum = tmp[0] + tmp[1];
#endif

    for (; i < len; ++i) {
        sum += a[i];
    }
    return sum;
}

double simd_centered_energy_double(const double* a, size_t len, double mean) {
    size_t i = 0;
    double sum = 0.0;

#if defined(__AVX2__)
    const size_t vec_end = len & ~static_cast<size_t>(7);
    const __m256d vmean = _mm256_set1_pd(mean);
    __m256d acc0 = _mm256_setzero_pd();
    __m256d acc1 = _mm256_setzero_pd();
    for (; i < vec_end; i += 8) {
        __m256d d0 = _mm256_sub_pd(_mm256_loadu_pd(a + i), vmean);
        __m256d d1 = _mm256_sub_pd(_mm256_loadu_pd(a + i + 4), vmean);
  #if defined(__FMA__)
        acc0 = _mm256_fmadd_pd(d0, d0, acc0);
        acc1 = _mm256_fmadd_pd(d1, d1, acc1);
  #else
        acc0 = _mm256_add_pd(acc0, _mm256_mul_pd(d0, d0));
        acc1 = _mm256_add_pd(acc1, _mm256_mul_pd(d1, d1));
  #endif
    }
    acc0 = _mm256_add_pd(acc0, acc1);
    alignas(32) double tmp[4];
    _mm256_store_pd(tmp, acc0);
    sum = (tmp[0] + tmp[1]) + (tmp[2] + tmp[3]);
#elif defined(__SSE2__)
    const size_t vec_end = len & ~static_cast<size_t>(3);
    const __m128d vmean = _mm_set1_pd(mean);
    __m128d acc0 = _mm_setzero_pd();
    __m128d acc1 = _mm_setzero_pd();
    for (; i < vec_end; i += 4) {
        __m128d d0 = _mm_sub_pd(_mm_loadu_pd(a + i), vmean);
        __m128d d1 = _mm_sub_pd(_mm_loadu_pd(a + i + 2), vmean);
        acc0 = _mm_add_pd(acc0, _mm_mul_pd(d0, d0));
        acc1 = _mm_add_pd(acc1, _mm_mul_pd(d1, d1));
    }
    acc0 = _mm_add_pd(acc0, acc1);
    alignas(16) double tmp[2];
    _mm_store_pd(tmp, acc0);
    sum = tmp[0] + tmp[1];
#endif

    for (; i < len; ++i) {
        const double d = a[i] - mean;
        sum += d * d;
    }
    return sum;
}

/* ============================================================================
 * SoftCorrelator Implementation
 * ============================================================================ */
SoftCorrelator::SoftCorrelator() : ncod_(0), sync_energy_(1.0), sync_tpl_sum_(0.0) {
    rebuild_templates();
}

void SoftCorrelator::set_codebook(int32_t ncod) {
    const int32_t normalized = (ncod == 1) ? 1 : 0;
    if (normalized != ncod_) {
        ncod_ = normalized;
        rebuild_templates();
    }
}

void SoftCorrelator::rebuild_templates() {
    /* Expand 128-chip Sync Preamble (4 x 32-bit words) onto DLPOLr = 13 grid -> 1664 samples */
    for (int w = 0; w < 4; ++w) {
        const uint32_t word = kGsp[static_cast<size_t>(ncod_)][static_cast<size_t>(w)];
        for (int bit = 0; bit < 32; ++bit) {
            const double val = ((word >> (31 - bit)) & 1u) ? 1.0 : -1.0;
            const size_t base = static_cast<size_t>((w * 32 + bit) * DSP_DLPOL_R);
            for (int s = 0; s < DSP_DLPOL_R; ++s) {
                sync_tpl_[base + static_cast<size_t>(s)] = val;
            }
        }
    }
    sync_energy_  = simd_energy_double(sync_tpl_.data(), DSP_SYNC_SAMPLES);
    sync_tpl_sum_ = simd_sum_double(sync_tpl_.data(), DSP_SYNC_SAMPLES);

    /* Expand 16 RM(1,4) orthogonal codewords (32 half-periods * 13 = 416 samples each) */
    for (int c = 0; c < DSP_RM14_CODES; ++c) {
        const uint32_t word = kGcodes[static_cast<size_t>(ncod_)][static_cast<size_t>(c)];
        auto& tpl = vec_tpls_[static_cast<size_t>(c)];
        for (int bit = 0; bit < 32; ++bit) {
            const double val = ((word >> (31 - bit)) & 1u) ? 1.0 : -1.0;
            const size_t base = static_cast<size_t>(bit * DSP_DLPOL_R);
            for (int s = 0; s < DSP_DLPOL_R; ++s) {
                tpl[base + static_cast<size_t>(s)] = val;
            }
        }
        vec_energies_[static_cast<size_t>(c)] = simd_energy_double(tpl.data(), DSP_VEC_SAMPLES);
    }
}

SyncSearchResult SoftCorrelator::search_sync_preamble(
    const double* buffer,
    size_t len,
    int32_t dcp_threshold
) const {
    SyncSearchResult res{false, 0, 1, 0, 0.0};
    if (!buffer || len < static_cast<size_t>(DSP_SYNC_SAMPLES)) {
        return res;
    }

    const size_t num_lags = len - static_cast<size_t>(DSP_SYNC_SAMPLES) + 1;
    double best_abs  = -1.0;
    double best_corr = 0.0;
    size_t best_lag  = 0;

    /* Zero-mean (DC-immune) SIMD Pearson correlation over 1664 doubles:
     * immune to ADC DC bias even when HPF/corrector is off or still settling. */
    const double tpl_sum = sync_tpl_sum_;
    constexpr double inv_sync_n = 1.0 / static_cast<double>(DSP_SYNC_SAMPLES);

    for (size_t lag = 0; lag < num_lags; ++lag) {
        const double* seg = buffer + lag;
        const double mean = simd_sum_double(seg, DSP_SYNC_SAMPLES) * inv_sync_n;
        const double xkv  = simd_centered_energy_double(seg, DSP_SYNC_SAMPLES, mean);
        if (xkv <= 1e-18) continue;

        const double dot  = simd_dot_double(seg, sync_tpl_.data(), DSP_SYNC_SAMPLES) - mean * tpl_sum;
        const double corr = dot / std::sqrt(sync_energy_ * xkv);
        const double a    = std::fabs(corr);
        if (a > best_abs) {
            best_abs  = a;
            best_corr = corr;
            best_lag  = lag;
        }
    }

    if (best_abs < 0.0) return res;

    int32_t pct = static_cast<int32_t>(std::lround(best_abs * 100.0));
    if (pct > 100) pct = 100;

    res.peak_index      = static_cast<int32_t>(best_lag);
    res.phase_polarity  = (best_corr >= 0.0) ? 1 : -1;
    res.reliability_pct = pct;
    res.peak_corr       = best_corr;
    res.found           = (pct >= dcp_threshold);
    return res;
}

int32_t SoftCorrelator::correlate_sync_at(const double* seg) const {
    if (!seg) return 0;
    constexpr double inv_sync_n = 1.0 / static_cast<double>(DSP_SYNC_SAMPLES);
    const double mean = simd_sum_double(seg, DSP_SYNC_SAMPLES) * inv_sync_n;
    const double xkv  = simd_centered_energy_double(seg, DSP_SYNC_SAMPLES, mean);
    if (xkv <= 1e-18) return 0;
    const double dot  = simd_dot_double(seg, sync_tpl_.data(), DSP_SYNC_SAMPLES) - mean * sync_tpl_sum_;
    const double corr = dot / std::sqrt(sync_energy_ * xkv);
    int32_t pct = static_cast<int32_t>(std::lround(corr * 100.0));
    if (pct > 100) pct = 100;
    if (pct < -100) pct = -100;
    return pct;
}

VectorDecodeResult SoftCorrelator::decode_vector(
    const double* window_ptr,
    int32_t search_ig,
    int32_t frame_phase
) const {
    VectorDecodeResult res{};
    if (!window_ptr) return res;

    const int32_t ig = std::max<int32_t>(0, search_ig);
    const double phase_sign = (frame_phase < 0) ? -1.0 : 1.0;
    constexpr double inv_vec_n = 1.0 / static_cast<double>(DSP_VEC_SAMPLES);

    /* Stage 1 (KORDAT2k.cpp lines 90-115):
     * Evaluate all 16 RM(1,4) templates at nominal offset `ig` (DC-centered) */
    const double* nominal_seg = window_ptr + ig;
    const double nominal_mean = simd_sum_double(nominal_seg, DSP_VEC_SAMPLES) * inv_vec_n;
    const double nominal_xkv  = simd_centered_energy_double(nominal_seg, DSP_VEC_SAMPLES, nominal_mean);
    if (nominal_xkv <= 1e-18) return res;

    int32_t best_code4 = 0;
    double  best_stage1_abs = -1.0;
    double  best_stage1_val = 0.0;

    for (int32_t c = 0; c < DSP_RM14_CODES; ++c) {
        const double dot = simd_dot_double(
            nominal_seg,
            vec_tpls_[static_cast<size_t>(c)].data(),
            DSP_VEC_SAMPLES
        );
        const double corr = (dot * phase_sign) / std::sqrt(vec_energies_[static_cast<size_t>(c)] * nominal_xkv);
        int32_t score_pct = static_cast<int32_t>(std::lround(corr * 100.0));
        if (score_pct > 100) score_pct = 100;
        if (score_pct < -100) score_pct = -100;
        res.code_scores_pct[c] = score_pct;

        const double a = std::fabs(corr);
        if (a > best_stage1_abs) {
            best_stage1_abs = a;
            best_stage1_val = corr;
            best_code4      = c;
        }
    }

    /* Stage 2 (KORDAT2k.cpp lines 117-138):
     * Fine sliding peak search across [0 .. 2*ig] using the winning RM(1,4) template */
    const auto& win_tpl = vec_tpls_[static_cast<size_t>(best_code4)];
    const double win_en = vec_energies_[static_cast<size_t>(best_code4)];

    int32_t best_pos = ig;
    double  best_abs = best_stage1_abs;
    double  best_val = best_stage1_val;

    if (ig > 0) {
        const int32_t total_steps = 2 * ig + 1;
        for (int32_t s = 0; s < total_steps; ++s) {
            const double* seg = window_ptr + s;
            const double mean = simd_sum_double(seg, DSP_VEC_SAMPLES) * inv_vec_n;
            const double xkv  = simd_centered_energy_double(seg, DSP_VEC_SAMPLES, mean);
            if (xkv <= 1e-18) continue;
            const double dot = simd_dot_double(seg, win_tpl.data(), DSP_VEC_SAMPLES);
            const double corr = (dot * phase_sign) / std::sqrt(win_en * xkv);
            const double a = std::fabs(corr);
            if (a > best_abs) {
                best_abs = a;
                best_val = corr;
                best_pos = s;
            }
        }
    }

    /* Phase inversion relative to Sync Preamble encodes 5th bit (+16) */
    uint8_t code5 = static_cast<uint8_t>(best_code4);
    if (best_val < 0.0) {
        code5 = static_cast<uint8_t>(code5 | 0x10u);
    }

    int32_t pct = static_cast<int32_t>(std::lround(best_abs * 100.0));
    if (pct > 100) pct = 100;

    res.code_5bit       = code5;
    res.reliability_pct = pct;
    res.peak_offset_ig  = best_pos - ig;
    res.peak_corr       = best_val;
    return res;
}

} // namespace dsp

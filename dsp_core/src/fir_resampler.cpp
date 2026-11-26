#include "fir_resampler.hpp"
#include "dsp_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace dsp {

/* ============================================================================
 * FirFilter Implementation (SIMD double-precision convolution)
 * ============================================================================ */
FirFilter::FirFilter() : head_(0) {
    set_passthrough();
}

int32_t FirFilter::load_from_file(const char* path) {
    if (!path || path[0] == '\0') {
        set_passthrough();
        return 0;
    }

#if defined(_WIN32)
    std::ifstream in(std::filesystem::u8path(path));
#else
    std::ifstream in(path);
#endif
    if (!in.is_open()) return DSP_ERR_FILE_OPEN;

    std::vector<double> coeffs;
    coeffs.reserve(4096);
    std::string line;
    while (std::getline(in, line)) {
        /* Handle legacy Windows comma/space formatting safely */
        for (char& ch : line) {
            if (ch == ',') ch = '.';
        }
        const char* p = line.c_str();
        char* end_ptr = nullptr;
        double val = std::strtod(p, &end_ptr);
        if (end_ptr != p) {
            coeffs.push_back(val);
        }
    }

    if (coeffs.empty()) return DSP_ERR_INVALID_ARG;
    set_coefficients(coeffs);
    return static_cast<int32_t>(taps_.size());
}

void FirFilter::set_coefficients(const std::vector<double>& taps) {
    if (taps.empty()) {
        set_passthrough();
        return;
    }
    /* Reverse taps so history_[head .. head + N - 1] aligns directly with dot(taps_, history) */
    taps_.assign(taps.rbegin(), taps.rend());
    history_.assign(taps_.size() * 2, 0.0);
    head_ = 0;
}

void FirFilter::set_passthrough() {
    taps_.clear();
    history_.clear();
    head_ = 0;
}

void FirFilter::reset() {
    std::fill(history_.begin(), history_.end(), 0.0);
    head_ = 0;
}

double FirFilter::process_sample(double x) {
    const size_t n = taps_.size();
    if (n == 0) return x;

    /* Store sample in doubled ring buffer so oldest..newest is always contiguous */
    history_[head_]     = x;
    history_[head_ + n] = x;

    head_ = (head_ + 1) % n;
    /* Contiguous chronological slice starts at history_[head_] (oldest) .. history_[head_ + n - 1] (newest) */
    return simd_dot_double(&history_[head_], taps_.data(), n);
}

void FirFilter::process_block(const double* in_x, double* out_y, size_t count) {
    if (taps_.empty()) {
        if (in_x != out_y) {
            std::copy(in_x, in_x + count, out_y);
        }
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        out_y[i] = process_sample(in_x[i]);
    }
}

/* ============================================================================
 * VirtualGridResampler Implementation (DLPOLr=13 + PIK5/ID Crystal Tracking)
 * ============================================================================ */
VirtualGridResampler::VirtualGridResampler()
    : nominal_step_h_(1.0),
      step_h_(1.0),
      frame_virtual_samples_(DSP_SYNC_SAMPLES + 39 * DSP_VEC_SAMPLES),
      frac_pos_(0.0),
      prev_sample_(0.0),
      has_prev_(false),
      pik5_{0, 0, 0, 0, 0},
      pik_count_(0),
      last_drift_id_(0) {}

void VirtualGridResampler::reset(double nominal_step_h, int32_t frame_virtual_samples) {
    nominal_step_h_        = (nominal_step_h > 1e-6) ? nominal_step_h : 1.0;
    step_h_                = nominal_step_h_;
    frame_virtual_samples_ = (frame_virtual_samples > DSP_SYNC_SAMPLES)
                                 ? frame_virtual_samples
                                 : (DSP_SYNC_SAMPLES + 39 * DSP_VEC_SAMPLES);
    frac_pos_              = 0.0;
    prev_sample_           = 0.0;
    has_prev_              = false;
    pik5_.fill(0);
    pik_count_             = 0;
    last_drift_id_         = 0;
}

void VirtualGridResampler::set_step_h(double h) {
    if (h > 1e-6) step_h_ = h;
}

void VirtualGridResampler::push_filtered(
    const double* filtered,
    size_t count,
    std::vector<double>& out_virtual
) {
    if (!filtered || count == 0) return;

    /*
     * Index mapping across block boundary:
     *   idx = -1 -> prev_sample_
     *   idx =  0 .. count-1 -> filtered[idx]
     * We interpolate between idx and idx+1 while frac_pos_ < (count - 1).
     */
    if (!has_prev_) {
        prev_sample_ = filtered[0];
        has_prev_    = true;
        frac_pos_    = 0.0;
    }

    auto sample_at = [&](int64_t idx) -> double {
        if (idx < 0) return prev_sample_;
        return filtered[static_cast<size_t>(idx)];
    };

    const double max_pos = static_cast<double>(count) - 1.0;
    while (frac_pos_ < max_pos) {
        const int64_t k = static_cast<int64_t>(std::floor(frac_pos_));
        const double alpha = frac_pos_ - static_cast<double>(k);
        const double s0 = sample_at(k);
        const double s1 = sample_at(k + 1);
        out_virtual.push_back(s0 + (s1 - s0) * alpha);
        frac_pos_ += step_h_;
    }

    /* Carry fractional position & last sample to next block */
    prev_sample_ = filtered[count - 1];
    frac_pos_   -= static_cast<double>(count);
}

bool VirtualGridResampler::report_sync_drift(int32_t drift_virtual_samples, bool enable_tracking) {
    /*
     * Direct port of PIK5 / ID drift compensation from TRAEKT2k.cpp lines 1593-1630.
     * Tracks last 5 sync-peak drift observations; when all 5 are within bounds and consistent,
     * adjusts step_h_ proportionally to compensate downhole quartz oscillator thermal drift.
     */
    const int32_t max_delta = std::max(40, frame_virtual_samples_ / 40);

    if (pik_count_ < 5) {
        pik5_[static_cast<size_t>(pik_count_)] = drift_virtual_samples;
        pik_count_++;
    } else {
        for (int i = 1; i < 5; ++i) {
            pik5_[static_cast<size_t>(i - 1)] = pik5_[static_cast<size_t>(i)];
        }
        pik5_[4] = drift_virtual_samples;
    }

    if (!enable_tracking || pik_count_ < 5) return false;
    if (std::abs(pik5_[4]) == 0 || std::abs(pik5_[3]) == 0) return false;

    for (int i = 0; i < 5; ++i) {
        if (std::abs(pik5_[static_cast<size_t>(i)]) >= max_delta) {
            return false;
        }
    }

    int32_t sum = 0;
    for (int i = 0; i < 5; ++i) sum += pik5_[static_cast<size_t>(i)];
    const int32_t avg_drift = static_cast<int32_t>(std::lround(static_cast<double>(sum) / 5.0));

    if (std::abs(avg_drift) < 2) return false;

    /*
     * Positive drift means the next sync preamble arrived LATER in virtual samples
     * -> our virtual grid is slightly too dense (step_h_ too small) -> increase step_h_.
     */
    const double current_input_frame = step_h_ * static_cast<double>(frame_virtual_samples_);
    const double adjusted_input_frame = current_input_frame + static_cast<double>(avg_drift) * step_h_;
    const double candidate_h = adjusted_input_frame / static_cast<double>(frame_virtual_samples_);

    /* Clamp total drift within ±3% of nominal step H for stability */
    const double min_h = nominal_step_h_ * 0.97;
    const double max_h = nominal_step_h_ * 1.03;
    if (candidate_h >= min_h && candidate_h <= max_h) {
        step_h_        = candidate_h;
        last_drift_id_ = avg_drift;
        pik5_.fill(0);
        pik_count_ = 0;
        return true;
    }
    return false;
}

} // namespace dsp

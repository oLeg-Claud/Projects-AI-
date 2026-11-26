#ifndef DSP_FIR_RESAMPLER_HPP
#define DSP_FIR_RESAMPLER_HPP

#include <cstddef>
#include <cstdint>
#include <vector>
#include <array>

namespace dsp {

/**
 * High-precision FIR filter with double-precision SIMD convolution.
 * Supports loading legacy KIH*.dat files (one scientific float per line, e.g. KIH5_4F.dat).
 */
class FirFilter {
public:
    FirFilter();

    /** Load coefficients from legacy KIH*.dat file. Returns tap count or < 0 on error. */
    int32_t load_from_file(const char* path);

    /** Set custom coefficients directly (or empty for passthrough). */
    void set_coefficients(const std::vector<double>& taps);

    /** Clear filter to passthrough mode (y[n] = x[n]). */
    void set_passthrough();

    void reset();
    size_t tap_count() const { return taps_.size(); }

    /** Process one input sample and return filtered double output. */
    double process_sample(double x);

    /** Process a block of input samples into out_y (out_y may alias in_x only if copied). */
    void process_block(const double* in_x, double* out_y, size_t count);

private:
    std::vector<double> taps_;
    /* Doubled circular history buffer so convolution is always a single contiguous SIMD dot product */
    std::vector<double> history_;
    size_t head_;
};

/**
 * Virtual-grid Linear Resampler (DLPOLr = 13 samples/half-period) +
 * PIK5 / ID Dynamic Step-H Crystal Drift Tracker (TRAEKT2k.cpp lines 1586-1630).
 */
class VirtualGridResampler {
public:
    VirtualGridResampler();

    void reset(double nominal_step_h, int32_t frame_virtual_samples);
    void set_step_h(double h);

    double step_h() const { return step_h_; }
    double nominal_step_h() const { return nominal_step_h_; }
    int32_t last_drift_id() const { return last_drift_id_; }

    /**
     * Feed filtered input samples and append resampled virtual-grid samples to out_virtual.
     * Linear interpolation matches YCO8_1.cpp line 318:
     *   masc[numc] = masf[k] + (masf[k+1] - masf[k]) * (x - k)
     */
    void push_filtered(const double* filtered, size_t count, std::vector<double>& out_virtual);

    /**
     * Feed observed sync preamble peak drift (in virtual samples relative to expected frame period)
     * into the 5-frame PIK5 median/average drift tracker.
     * Returns true if step H was adjusted to compensate quartz crystal drift.
     */
    bool report_sync_drift(int32_t drift_virtual_samples, bool enable_tracking);

private:
    double  nominal_step_h_;
    double  step_h_;
    int32_t frame_virtual_samples_;
    double  frac_pos_;     /* Fractional read position relative to buffer start */
    double  prev_sample_;
    bool    has_prev_;

    /* PIK5 drift history (TRAEKT2k.cpp npik = 5) */
    std::array<int32_t, 5> pik5_;
    int32_t pik_count_;
    int32_t last_drift_id_;
};

} // namespace dsp

#endif /* DSP_FIR_RESAMPLER_HPP */

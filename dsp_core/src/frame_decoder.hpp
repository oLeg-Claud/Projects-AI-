#ifndef DSP_FRAME_DECODER_HPP
#define DSP_FRAME_DECODER_HPP

#include <array>
#include <cstdint>
#include <functional>
#include "../include/dsp_core_api.h"

namespace dsp {

/**
 * Vector-by-vector telemetry frame decoder faithfully ported from KORDAT2k.cpp & format*_2k.h.
 * Supports all 7 legacy formats (R1, R2, R11, R12, R13, R30, R31), repeater pauses,
 * Geomash 5-bit->8-bit packing + CRC32 verification, logarithmic GK scaling,
 * standard + NUBT azimuth solver, and Zero-Latency OnToolfaceCallback firing
 * the instant an OTK*_L / AZ_OTK*_L / OTKu_L vector is decoded.
 */
class FrameDecoder {
public:
    using ToolfaceSink = std::function<void(const DspToolfaceEvent&)>;

    FrameDecoder();

    void configure(const DspConfig& cfg);
    void begin_frame(
        int32_t frame_number,
        int32_t phase_polarity,
        int32_t sync_corr_pct,
        double step_h,
        int32_t last_pik_drift
    );

    /**
     * Returns true if the given physical vector slot (0..total_physical_vectors-1)
     * is a repeater pause slot that should be skipped without decoding data.
     */
    bool is_pause_slot(int32_t phys_slot) const;

    /**
     * Maps a physical vector index (including pause slots) to its logical `kod` index
     * matching format*_2k.h, or returns -1 if `phys_slot` is a pause slot.
     */
    int32_t physical_to_logical_kod(int32_t phys_slot) const;

    /** Total physical vectors in one frame (base data vectors + repeater pause vectors). */
    int32_t total_physical_vectors() const;

    /**
     * Ingest one decoded logical vector `kod` immediately after correlation.
     * Fires `toolface_sink` immediately (< 1 ms) whenever an OTK_L / AZ_OTK_L / OTKu_L vector arrives.
     */
    void process_logical_vector(
        int32_t kod,
        uint8_t code_5bit,
        int32_t reliability_pct,
        double decode_latency_ms,
        const ToolfaceSink& toolface_sink
    );

    /** Finalize end-of-frame computations (e.g., Geomash CRC32 payload). */
    void finish_frame();

    const DspFrameData& current_frame() const { return frame_; }

private:
    void compute_surface_inclinometry(int32_t hx_raw, int32_t hy_raw, int32_t hz_raw);
    void apply_service_param();
    void emit_toolface(
        DspToolfaceChannel ch,
        int32_t vec_idx,
        double angle_deg,
        int32_t rel_pct,
        double latency_ms,
        const ToolfaceSink& sink
    );

    DspConfig cfg_;
    DspFrameData frame_;

    std::array<uint8_t, DSP_MAX_VECTORS> dig_;
    std::array<uint8_t, DSP_MAX_VECTORS> rel_;

    /* Intermediate raw accumulators inside current frame */
    int32_t hx_raw_;
    int32_t hy_raw_;
    int32_t hz_raw_;
    int32_t servis_raw_;
    int32_t geomash_count_;
    int32_t pause_vecs_per_slot_;
    int32_t base_vectors_;
};

} // namespace dsp

#endif /* DSP_FRAME_DECODER_HPP */

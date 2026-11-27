#include "../include/dsp_core_api.h"
#include "dsp_internal.hpp"
#include "uso_source.hpp"
#include "fir_resampler.hpp"
#include "correlator.hpp"
#include "frame_decoder.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct PendingCallbacks {
    std::vector<double>           raw_copy;
    std::vector<double>           filtered_copy;
    int32_t                       overflow_flag{0};
    bool                          has_signal{false};
    std::vector<DspVectorEvent>   vectors;
    std::vector<DspToolfaceEvent> toolfaces;
    std::vector<DspFrameData>     frames;
};

struct DspEngine {
    enum class State {
        SearchingSync = 0,
        DecodingVectors = 1
    };

    DspConfig       cfg{};
    DspUsoV2Options v2_opts{};
    DspUsoRuntimeInfo cached_uso_info{};
    DspCallbacks    callbacks{};
    dsp::FirFilter            fir;
    dsp::VirtualGridResampler resampler;
    dsp::SoftCorrelator       correlator;
    dsp::FrameDecoder         decoder;

    std::unique_ptr<dsp::IUsoSource> source;
    std::thread       worker;
    std::atomic<bool> running{false};
    std::mutex        mutex; /* Protects DSP state & pending flags ONLY — NEVER held during I/O or callbacks */
    std::condition_variable sync_op_cv;

    /* Pending hardware operations executed exclusively by worker thread (zero UI blocking) */
    bool     pending_gain{false};
    int32_t  pending_gain_val{0};
    bool     pending_corr{false};
    bool     pending_corr_val{true};
    bool     pending_v2_opts{false};
    bool     pending_reconfig{false};
    bool     pending_diag{false};
    bool     pending_recal{false};
    bool     pending_reset_adc{false};
    bool     sync_op_done{false};
    int32_t  sync_op_result{DSP_OK};

    /* Streaming DSP buffers */
    std::vector<double> filtered_scratch;
    std::vector<double> virtual_buf;
    State    state{State::SearchingSync};
    int32_t  frame_number{0};
    int32_t  frame_phase{1};
    int32_t  phys_vec_cursor{0};
    int32_t  total_phys_vecs{39};
    size_t   next_vec_pos{0};
    int64_t  total_virtual_consumed{0};
    int64_t  last_sync_abs_pos{-1};

    void rebuild_pipeline_locked() {
        correlator.set_codebook(cfg.ncod);
        decoder.configure(cfg);
        total_phys_vecs = decoder.total_physical_vectors();
        const int32_t frame_virt = DSP_SYNC_SAMPLES + total_phys_vecs * DSP_VEC_SAMPLES;
        const double nom_h = dsp::compute_nominal_step_h(
            cfg.sample_rate_hz,
            cfg.carrier_freq_hz,
            cfg.source_type
        );
        resampler.reset(nom_h, frame_virt);
        fir.reset();
        virtual_buf.clear();
        state                  = State::SearchingSync;
        phys_vec_cursor        = 0;
        next_vec_pos           = 0;
        total_virtual_consumed = 0;
        last_sync_abs_pos      = -1;
    }

    void discard_prefix(size_t n) {
        if (n == 0) return;
        if (n >= virtual_buf.size()) {
            total_virtual_consumed += static_cast<int64_t>(virtual_buf.size());
            virtual_buf.clear();
        } else {
            virtual_buf.erase(virtual_buf.begin(), virtual_buf.begin() + static_cast<std::ptrdiff_t>(n));
            total_virtual_consumed += static_cast<int64_t>(n);
        }
    }

    void process_virtual_buffer_locked(PendingCallbacks& out_cb) {
        const int32_t ig = std::max<int32_t>(0, cfg.search_window_ig);
        const int32_t edge_margin = std::max<int32_t>(ig, 16);
        const int32_t frame_virt = DSP_SYNC_SAMPLES + total_phys_vecs * DSP_VEC_SAMPLES;

        bool progress = true;
        while (progress) {
            progress = false;

            if (state == State::SearchingSync) {
                /* Wait until we have at least one sync preamble + search margin */
                const size_t min_search = static_cast<size_t>(DSP_SYNC_SAMPLES + 2 * edge_margin);
                if (virtual_buf.size() < min_search) break;

                /* Search within first chunk (bounded to keep latency low) */
                const size_t search_len = std::min<size_t>(
                    virtual_buf.size(),
                    static_cast<size_t>(DSP_SYNC_SAMPLES + DSP_VEC_SAMPLES)
                );
                const size_t num_lags = search_len - static_cast<size_t>(DSP_SYNC_SAMPLES) + 1;

                const auto sync_res = correlator.search_sync_preamble(
                    virtual_buf.data(),
                    search_len,
                    cfg.dcp_threshold
                );

                if (sync_res.found) {
                    /* If peak sits right on the trailing edge of search_len, wait for `edge_margin`
                     * more virtual samples so we lock onto the exact crest instead of its rising shoulder. */
                    if (num_lags > static_cast<size_t>(edge_margin) &&
                        sync_res.peak_index >= static_cast<int32_t>(num_lags) - edge_margin &&
                        virtual_buf.size() < search_len + static_cast<size_t>(edge_margin)) {
                        const size_t safe_drop = (sync_res.peak_index > edge_margin)
                                                     ? static_cast<size_t>(sync_res.peak_index - edge_margin)
                                                     : 0;
                        if (safe_drop > 0) {
                            discard_prefix(safe_drop);
                        }
                        break;
                    }

                    const int64_t abs_sync = total_virtual_consumed + sync_res.peak_index;
                    if (last_sync_abs_pos >= 0) {
                        const int64_t measured_period = abs_sync - last_sync_abs_pos;
                        const int32_t drift = static_cast<int32_t>(measured_period - frame_virt);
                        if (std::abs(drift) < frame_virt / 4) {
                            resampler.report_sync_drift(drift, cfg.enable_step_track != 0);
                        }
                    }
                    last_sync_abs_pos = abs_sync;

                    frame_number++;
                    frame_phase     = sync_res.phase_polarity;
                    phys_vec_cursor = 0;

                    decoder.begin_frame(
                        frame_number,
                        frame_phase,
                        sync_res.reliability_pct,
                        resampler.step_h(),
                        resampler.last_drift_id()
                    );

                    /* Position right after Sync Preamble, leaving `ig` guard samples before nominal vector 0 */
                    const size_t vec0_start = static_cast<size_t>(sync_res.peak_index + DSP_SYNC_SAMPLES);
                    if (vec0_start > static_cast<size_t>(ig)) {
                        discard_prefix(vec0_start - static_cast<size_t>(ig));
                        next_vec_pos = static_cast<size_t>(ig);
                    } else {
                        next_vec_pos = vec0_start;
                    }

                    state    = State::DecodingVectors;
                    progress = true;
                } else {
                    /* Advance buffer past evaluated lags */
                    const size_t step_drop = search_len - static_cast<size_t>(DSP_SYNC_SAMPLES) + 1;
                    discard_prefix(step_drop);
                    progress = (virtual_buf.size() >= min_search);
                }
            }

            if (state == State::DecodingVectors) {
                while (phys_vec_cursor < total_phys_vecs) {
                    const size_t required_end = next_vec_pos + static_cast<size_t>(DSP_VEC_SAMPLES + ig);
                    if (virtual_buf.size() < required_end) {
                        break;
                    }

                    const int32_t logical_kod = decoder.physical_to_logical_kod(phys_vec_cursor);
                    if (logical_kod >= 0) {
                        const auto t0 = std::chrono::steady_clock::now();
                        const size_t win_start = (next_vec_pos >= static_cast<size_t>(ig))
                                                     ? (next_vec_pos - static_cast<size_t>(ig))
                                                     : 0;
                        const int32_t eff_ig = static_cast<int32_t>(next_vec_pos - win_start);

                        const auto vres = correlator.decode_vector(
                            virtual_buf.data() + win_start,
                            eff_ig,
                            frame_phase
                        );
                        const double lat_ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0
                        ).count();

                        if (callbacks.on_vector) {
                            DspVectorEvent vevt{};
                            vevt.frame_number    = frame_number;
                            vevt.vector_index    = logical_kod;
                            vevt.total_vectors   = total_phys_vecs;
                            vevt.code_5bit       = vres.code_5bit;
                            vevt.reliability_pct = vres.reliability_pct;
                            vevt.peak_offset_ig  = vres.peak_offset_ig;
                            vevt.phase_polarity  = frame_phase;
                            std::memcpy(vevt.code_scores_pct, vres.code_scores_pct, sizeof(vevt.code_scores_pct));
                            out_cb.vectors.push_back(vevt);
                        }

                        /* Feed logical vector into FrameDecoder -> ZERO-LATENCY OnToolfaceCallback */
                        decoder.process_logical_vector(
                            logical_kod,
                            vres.code_5bit,
                            vres.reliability_pct,
                            lat_ms,
                            [&](const DspToolfaceEvent& tf_evt) {
                                if (callbacks.on_toolface) {
                                    out_cb.toolfaces.push_back(tf_evt);
                                }
                            }
                        );

                        /* Track fine timing offset (sdvig) exactly like OnMyPost2.h line 89 */
                        const int64_t next_pos_signed =
                            static_cast<int64_t>(next_vec_pos) +
                            static_cast<int64_t>(DSP_VEC_SAMPLES) +
                            static_cast<int64_t>(vres.peak_offset_ig);
                        next_vec_pos = (next_pos_signed > 0) ? static_cast<size_t>(next_pos_signed) : 0;
                    } else {
                        /* Repeater pause slot: skip exactly one vector duration */
                        next_vec_pos += static_cast<size_t>(DSP_VEC_SAMPLES);
                    }

                    phys_vec_cursor++;

                    /* Periodically trim consumed prefix to bound memory */
                    if (next_vec_pos > static_cast<size_t>(ig + DSP_VEC_SAMPLES * 4)) {
                        const size_t drop = next_vec_pos - static_cast<size_t>(ig);
                        discard_prefix(drop);
                        next_vec_pos -= drop;
                    }
                }

                if (phys_vec_cursor >= total_phys_vecs) {
                    decoder.finish_frame();
                    if (callbacks.on_frame) {
                        out_cb.frames.push_back(decoder.current_frame());
                    }
                    /* Leave a small guard before searching for next Sync Preamble */
                    if (next_vec_pos > static_cast<size_t>(ig)) {
                        const size_t drop = next_vec_pos - static_cast<size_t>(ig);
                        discard_prefix(drop);
                    }
                    next_vec_pos = 0;
                    state        = State::SearchingSync;
                    progress     = (virtual_buf.size() >= static_cast<size_t>(DSP_SYNC_SAMPLES));
                }
            }
        }
    }

    void push_samples_locked(const double* samples, int32_t count, int32_t overflow_flag, PendingCallbacks& out_cb) {
        if (!samples || count <= 0) return;
        // Never correlate across a transport gap or an invalid ADC block.
        if (overflow_flag != 0) rebuild_pipeline_locked();

        filtered_scratch.resize(static_cast<size_t>(count));
        fir.process_block(samples, filtered_scratch.data(), static_cast<size_t>(count));

        if (callbacks.on_signal) {
            out_cb.raw_copy.assign(samples, samples + count);
            out_cb.filtered_copy = filtered_scratch;
            out_cb.overflow_flag = overflow_flag;
            out_cb.has_signal    = true;
        }

        if (overflow_flag & dsp::usov2::Receiver::Overflow) {
            rebuild_pipeline_locked();
            return;
        }
        resampler.push_filtered(filtered_scratch.data(), static_cast<size_t>(count), virtual_buf);
        process_virtual_buffer_locked(out_cb);
    }

    void dispatch_callbacks_unlocked(const PendingCallbacks& cb) const {
        if (cb.has_signal && callbacks.on_signal) {
            DspSignalBlock blk{};
            blk.raw_samples      = cb.raw_copy.data();
            blk.filtered_samples = cb.filtered_copy.data();
            blk.sample_count     = static_cast<int32_t>(cb.raw_copy.size());
            blk.overflow_flag    = cb.overflow_flag;
            callbacks.on_signal(&blk, callbacks.user_data);
        }
        if (callbacks.on_vector) {
            for (const auto& vevt : cb.vectors) {
                callbacks.on_vector(&vevt, callbacks.user_data);
            }
        }
        if (callbacks.on_toolface) {
            for (const auto& tevt : cb.toolfaces) {
                callbacks.on_toolface(&tevt, callbacks.user_data);
            }
        }
        if (callbacks.on_frame) {
            for (const auto& fr : cb.frames) {
                callbacks.on_frame(&fr, callbacks.user_data);
            }
        }
    }
};

/* ============================================================================
 * Public C ABI Functions
 * ============================================================================ */
extern "C" {

DSPCORE_API void DSPCORE_CALL dsp_config_default(DspConfig* cfg) {
    if (!cfg) return;
    std::memset(cfg, 0, sizeof(DspConfig));
    cfg->source_type       = DSP_SRC_LEGACY_SERIAL;
    cfg->uso_bit_mode      = 32;
    cfg->gain_index        = 2;
    cfg->carrier_freq_hz   = 5.0;
    cfg->sample_rate_hz    = 250.0;
    cfg->corrector_enabled = 1;
    cfg->format_id         = DSP_FMT_STD_GK;
    cfg->ncod              = 0;
    cfg->search_window_ig  = 13;
    cfg->dcp_threshold     = 55;
    cfg->enable_step_track = 1;
    cfg->playback_speed    = 1;
    cfg->ref_hop_nt        = 46500.0;
    cfg->ref_dip_deg       = 71.0;
    cfg->ref_dec_deg       = 0.1;
    cfg->ref_az_op_deg     = 224.5;
    cfg->kgk_zts           = 1.0;
    cfg->use_nubt          = 0;
}

DSPCORE_API DspEngine* DSPCORE_CALL dsp_create(const DspConfig* cfg, const DspCallbacks* callbacks) {
    auto* eng = new (std::nothrow) DspEngine();
    if (!eng) return nullptr;

    if (cfg) {
        eng->cfg = *cfg;
    } else {
        dsp_config_default(&eng->cfg);
    }
    dsp_uso_v2_options_default(&eng->v2_opts);
    eng->cached_uso_info.active_sample_rate_hz = eng->cfg.sample_rate_hz;

    if (callbacks) {
        eng->callbacks = *callbacks;
    }

    if (eng->cfg.fir_file_path[0] != '\0') {
        eng->fir.load_from_file(eng->cfg.fir_file_path);
    }

    eng->rebuild_pipeline_locked();
    return eng;
}

DSPCORE_API void DSPCORE_CALL dsp_destroy(DspEngine* engine) {
    if (!engine) return;
    dsp_stop(engine);
    delete engine;
}

DSPCORE_API int32_t DSPCORE_CALL dsp_configure(DspEngine* engine, const DspConfig* cfg) {
    if (!engine || !cfg) return DSP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(engine->mutex);

    const bool is_live_serial = engine->running.load() &&
                                (engine->cfg.source_type == DSP_SRC_LEGACY_SERIAL) &&
                                (cfg->source_type == DSP_SRC_LEGACY_SERIAL);

    /* Never allow a stale caller cfg->sample_rate_hz (e.g. 250.0) to clobber
     * the hardware-negotiated sample rate of a running USO32 stream! */
    const double preserved_fs = is_live_serial ? engine->cfg.sample_rate_hz : cfg->sample_rate_hz;

    const bool freq_changed = (std::fabs(engine->cfg.carrier_freq_hz - cfg->carrier_freq_hz) > 1e-4);
    const bool gain_changed = (engine->cfg.gain_index != cfg->gain_index);
    const bool corr_changed = (engine->cfg.corrector_enabled != cfg->corrector_enabled);
    const bool need_reset =
        (engine->cfg.format_id != cfg->format_id) ||
        (engine->cfg.ncod != cfg->ncod) ||
        freq_changed ||
        (std::fabs(engine->cfg.sample_rate_hz - preserved_fs) > 1e-4) ||
        (engine->cfg.source_type != cfg->source_type);

    engine->cfg = *cfg;
    engine->cfg.sample_rate_hz = preserved_fs;
    engine->decoder.configure(engine->cfg);

    if (engine->running.load()) {
        /* Queue hardware operations for the worker thread — returns in <50 us, zero UI freeze */
        if (freq_changed && engine->cfg.source_type == DSP_SRC_LEGACY_SERIAL) {
            engine->pending_reconfig = true;
        } else {
            if (gain_changed) {
                engine->pending_gain     = true;
                engine->pending_gain_val = cfg->gain_index;
            }
            if (corr_changed) {
                engine->pending_corr     = true;
                engine->pending_corr_val = (cfg->corrector_enabled != 0);
            }
        }
    } else if (engine->source && engine->source->is_open()) {
        if (gain_changed) engine->source->set_gain(cfg->gain_index);
        if (corr_changed) engine->source->set_corrector(cfg->corrector_enabled != 0);
    }

    if (need_reset) {
        engine->rebuild_pipeline_locked();
    }
    return DSP_OK;
}

DSPCORE_API int32_t DSPCORE_CALL dsp_load_fir_file(DspEngine* engine, const char* path) {
    if (!engine) return DSP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(engine->mutex);
    return engine->fir.load_from_file(path);
}

DSPCORE_API int32_t DSPCORE_CALL dsp_start(DspEngine* engine) {
    if (!engine) return DSP_ERR_INVALID_ARG;
    if (engine->running.load()) return DSP_ERR_ALREADY_RUNNING;

    DspConfig local_cfg{};
    DspUsoV2Options local_v2{};
    {
        std::lock_guard<std::mutex> lock(engine->mutex);
        local_cfg = engine->cfg;
        local_v2  = engine->v2_opts;
    }

    std::unique_ptr<dsp::IUsoSource> src;
    switch (local_cfg.source_type) {
        case DSP_SRC_LEGACY_SERIAL:
            src = std::make_unique<dsp::LegacySerialUsoSource>();
            break;
        case DSP_SRC_NEW_ADC32:
            src = std::make_unique<dsp::NewAdc32UsoSource>();
            break;
        case DSP_SRC_FILE_PLAYER:
            src = std::make_unique<dsp::SmpFilePlayerSource>();
            break;
        default:
            return DSP_ERR_INVALID_ARG;
    }

    /* Perform blocking serial/file open OUTSIDE engine->mutex */
    src->set_v2_options(local_v2);
    int32_t rc = src->open(local_cfg);
    if (rc != DSP_OK) return rc;

    DspUsoRuntimeInfo init_info{};
    src->get_runtime_info(init_info);
    const double neg_fs = src->effective_sample_rate_hz();

    {
        std::lock_guard<std::mutex> lock(engine->mutex);
        if (neg_fs > 1.0) {
            engine->cfg.sample_rate_hz = neg_fs;
            init_info.active_sample_rate_hz = neg_fs;
        }
        engine->cached_uso_info   = init_info;
        engine->source            = std::move(src);
        engine->pending_gain      = false;
        engine->pending_corr      = false;
        engine->pending_v2_opts   = false;
        engine->pending_reconfig  = false;
        engine->pending_diag      = false;
        engine->pending_recal     = false;
        engine->pending_reset_adc = false;
        engine->rebuild_pipeline_locked();
    }

    engine->running.store(true);
    engine->worker = std::thread([engine]() {
        /* 16 samples @ 250..400 Hz = 40..64 ms per chunk; serial I/O runs WITHOUT holding engine->mutex! */
        std::vector<double> chunk(16, 0.0);
        while (engine->running.load()) {
            /* 1. Grab any pending hardware requests under a ~2 us lock */
            bool do_v2_opts = false;
            bool do_reconfig = false;
            bool do_gain = false;
            bool do_corr = false;
            bool do_diag = false;
            bool do_recal = false;
            bool do_reset_adc = false;
            int32_t gain_val = 0;
            bool corr_val = true;
            DspConfig reconfig_cfg{};
            DspUsoV2Options new_v2{};
            int32_t src_type = DSP_SRC_LEGACY_SERIAL;
            dsp::IUsoSource* src_ptr = nullptr;

            {
                std::lock_guard<std::mutex> lock(engine->mutex);
                src_ptr  = engine->source.get();
                src_type = engine->cfg.source_type;
                if (!src_ptr) break;

                if (engine->pending_v2_opts) {
                    do_v2_opts = true;
                    new_v2     = engine->v2_opts;
                    engine->pending_v2_opts = false;
                }
                if (engine->pending_reconfig) {
                    do_reconfig  = true;
                    reconfig_cfg = engine->cfg;
                    engine->pending_reconfig = false;
                    engine->pending_gain     = false;
                    engine->pending_corr     = false;
                } else {
                    if (engine->pending_gain) {
                        do_gain  = true;
                        gain_val = engine->pending_gain_val;
                        engine->pending_gain = false;
                    }
                    if (engine->pending_corr) {
                        do_corr  = true;
                        corr_val = engine->pending_corr_val;
                        engine->pending_corr = false;
                    }
                }
                if (engine->pending_diag) {
                    do_diag = true;
                    engine->pending_diag = false;
                }
                if (engine->pending_recal) {
                    do_recal = true;
                    engine->pending_recal = false;
                }
                if (engine->pending_reset_adc) {
                    do_reset_adc = true;
                    engine->pending_reset_adc = false;
                }
            }

            if (!src_ptr->is_open()) break;

            /* 2. Execute hardware commands OUTSIDE engine->mutex */
            if (do_v2_opts) {
                src_ptr->set_v2_options(new_v2);
            }
            if (do_reconfig) {
                (void)src_ptr->reconfigure(reconfig_cfg);
                const double new_fs = src_ptr->effective_sample_rate_hz();
                DspUsoRuntimeInfo updated_info{};
                src_ptr->get_runtime_info(updated_info);
                std::lock_guard<std::mutex> lock(engine->mutex);
                if (new_fs > 1.0) {
                    engine->cfg.sample_rate_hz = new_fs;
                    updated_info.active_sample_rate_hz = new_fs;
                }
                engine->cached_uso_info = updated_info;
                engine->rebuild_pipeline_locked();
            } else {
                if (do_gain) (void)src_ptr->set_gain(gain_val);
                if (do_corr) (void)src_ptr->set_corrector(corr_val);
            }
            if (do_diag) {
                (void)src_ptr->refresh_diagnostics();
            }
            if (do_recal) {
                int32_t op_rc = src_ptr->recalibrate();
                std::lock_guard<std::mutex> lock(engine->mutex);
                engine->sync_op_result = op_rc;
                engine->sync_op_done   = true;
                engine->sync_op_cv.notify_all();
            }
            if (do_reset_adc) {
                int32_t op_rc = src_ptr->reset_adc();
                std::lock_guard<std::mutex> lock(engine->mutex);
                engine->sync_op_result = op_rc;
                engine->sync_op_done   = true;
                engine->sync_op_cv.notify_all();
            }

            /* 3. Blocking read from hardware / .smp file OUTSIDE engine->mutex */
            int32_t overflow = 0;
            int32_t got = src_ptr->read_samples(
                chunk.data(),
                static_cast<int32_t>(chunk.size()),
                &overflow
            );

            DspUsoRuntimeInfo live_info{};
            src_ptr->get_runtime_info(live_info);

            if (got > 0) {
                PendingCallbacks pending_cb;
                {
                    std::lock_guard<std::mutex> lock(engine->mutex);
                    engine->cached_uso_info = live_info;
                    if (live_info.active_sample_rate_hz > 1.0 &&
                        std::fabs(engine->cfg.sample_rate_hz - live_info.active_sample_rate_hz) > 0.25 &&
                        engine->cfg.source_type == DSP_SRC_LEGACY_SERIAL) {
                        engine->cfg.sample_rate_hz = live_info.active_sample_rate_hz;
                        engine->rebuild_pipeline_locked();
                    }
                    engine->push_samples_locked(chunk.data(), got, overflow, pending_cb);
                }
                /* 4. Fire C# callbacks OUTSIDE engine->mutex */
                engine->dispatch_callbacks_unlocked(pending_cb);
            } else {
                {
                    std::lock_guard<std::mutex> lock(engine->mutex);
                    engine->cached_uso_info = live_info;
                }
                if (src_type == DSP_SRC_FILE_PLAYER) {
                    engine->running.store(false);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        /* Wake any caller waiting on a synchronous calibration/reset if stopped */
        std::lock_guard<std::mutex> lock(engine->mutex);
        if (!engine->sync_op_done) {
            engine->sync_op_result = DSP_ERR_NOT_FOUND;
            engine->sync_op_done   = true;
            engine->sync_op_cv.notify_all();
        }
    });

    return DSP_OK;
}

DSPCORE_API void DSPCORE_CALL dsp_stop(DspEngine* engine) {
    if (!engine) return;
    engine->running.store(false);
    if (engine->worker.joinable()) {
        engine->worker.join();
    }
    std::unique_ptr<dsp::IUsoSource> to_close;
    {
        std::lock_guard<std::mutex> lock(engine->mutex);
        to_close = std::move(engine->source);
    }
    if (to_close) {
        to_close->close();
    }
}

DSPCORE_API int32_t DSPCORE_CALL dsp_push_samples(
    DspEngine* engine,
    const double* samples,
    int32_t count,
    int32_t overflow_flag
) {
    if (!engine || !samples || count <= 0) return DSP_ERR_INVALID_ARG;
    PendingCallbacks pending_cb;
    {
        std::lock_guard<std::mutex> lock(engine->mutex);
        engine->push_samples_locked(samples, count, overflow_flag, pending_cb);
    }
    engine->dispatch_callbacks_unlocked(pending_cb);
    return DSP_OK;
}

DSPCORE_API int32_t DSPCORE_CALL dsp_get_last_frame(DspEngine* engine, DspFrameData* out_frame) {
    if (!engine || !out_frame) return DSP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(engine->mutex);
    *out_frame = engine->decoder.current_frame();
    return DSP_OK;
}

DSPCORE_API double DSPCORE_CALL dsp_get_step_h(const DspEngine* engine) {
    if (!engine) return 1.0;
    return engine->resampler.step_h();
}

DSPCORE_API const char* DSPCORE_CALL dsp_get_simd_isa(void) {
    return dsp::active_simd_isa_name();
}

DSPCORE_API int32_t DSPCORE_CALL dsp_is_running(const DspEngine* engine) {
    if (!engine) return 0;
    return engine->running.load() ? 1 : 0;
}

DSPCORE_API int32_t DSPCORE_CALL dsp_probe_legacy_uso(const char* port_name, int32_t* out_uso_id) {
    return dsp::LegacySerialUsoSource::probe_port(port_name, out_uso_id);
}

DSPCORE_API int32_t DSPCORE_CALL dsp_scan_legacy_uso(
    char* out_port_buf,
    int32_t buf_len,
    int32_t* out_uso_id
) {
    return dsp::LegacySerialUsoSource::scan_ports(out_port_buf, buf_len, out_uso_id);
}

DSPCORE_API int32_t DSPCORE_CALL dsp_scan_legacy_uso_diag(
    char* out_port_buf,
    int32_t buf_len,
    int32_t* out_uso_id,
    char* out_diag_buf,
    int32_t diag_len
) {
    return dsp::LegacySerialUsoSource::scan_ports_diag(out_port_buf, buf_len, out_uso_id, out_diag_buf, diag_len);
}

DSPCORE_API int32_t DSPCORE_CALL dsp_parse_legacy_packet(
    const uint8_t* packet,
    int32_t packet_len,
    int32_t bit_mode,
    int32_t* out_sample
) {
    return dsp::LegacySerialUsoSource::parse_packet(packet, packet_len, bit_mode, out_sample);
}

DSPCORE_API void DSPCORE_CALL dsp_uso_v2_options_default(DspUsoV2Options* opts) {
    if (!opts) return;
    std::memset(opts, 0, sizeof(DspUsoV2Options));
    opts->adaptive_rate     = 1;
    opts->dr_code           = dsp::usov2::DR_400;
    opts->decimation        = 0; /* auto */
    opts->adc_filter        = dsp::usov2::FLT_SINC4;
    opts->chop_enabled      = 0;
    opts->vbias_enabled     = 1;
    opts->inpmux            = 0x89;
    opts->mcu_lpf_mode      = dsp::usov2::LPF_BUTTER4; /* Adaptive on-MCU LPF (no PC KIH*.dat needed) */
    opts->mcu_lpf_cutoff_hz = 0.0; /* auto ~2.5 * carrier */
    opts->mcu_hpf_cutoff_hz = 0.1;
    opts->batch_size        = 0; /* auto */
    opts->auto_recover      = 1;
}

DSPCORE_API int32_t DSPCORE_CALL dsp_set_uso_v2_options(DspEngine* engine, const DspUsoV2Options* opts) {
    if (!engine || !opts) return DSP_ERR_INVALID_ARG;
    if (opts->chop_enabled) return DSP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(engine->mutex);
    const bool changed = (std::memcmp(&engine->v2_opts, opts, sizeof(DspUsoV2Options)) != 0);
    engine->v2_opts = *opts;
    if (engine->running.load()) {
        if (changed) {
            engine->pending_v2_opts  = true;
            engine->pending_reconfig = true;
        }
    } else if (engine->source) {
        engine->source->set_v2_options(*opts);
    }
    return DSP_OK;
}

DSPCORE_API int32_t DSPCORE_CALL dsp_get_uso_info(DspEngine* engine, DspUsoRuntimeInfo* out_info) {
    if (!engine || !out_info) return DSP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(engine->mutex);
    *out_info = engine->cached_uso_info;
    if (out_info->active_sample_rate_hz <= 1.0) {
        out_info->active_sample_rate_hz = engine->cfg.sample_rate_hz;
    }
    return DSP_OK;
}

DSPCORE_API int32_t DSPCORE_CALL dsp_uso_recalibrate(DspEngine* engine) {
    if (!engine) return DSP_ERR_INVALID_ARG;
    std::unique_lock<std::mutex> lock(engine->mutex);
    if (!engine->running.load() || !engine->source) return DSP_ERR_NOT_FOUND;
    engine->sync_op_done  = false;
    engine->pending_recal = true;
    if (!engine->sync_op_cv.wait_for(lock, std::chrono::seconds(5), [&]() { return engine->sync_op_done; })) {
        return DSP_ERR_PORT_OPEN;
    }
    return engine->sync_op_result;
}

DSPCORE_API int32_t DSPCORE_CALL dsp_uso_reset_adc(DspEngine* engine) {
    if (!engine) return DSP_ERR_INVALID_ARG;
    std::unique_lock<std::mutex> lock(engine->mutex);
    if (!engine->running.load() || !engine->source) return DSP_ERR_NOT_FOUND;
    engine->sync_op_done      = false;
    engine->pending_reset_adc = true;
    if (!engine->sync_op_cv.wait_for(lock, std::chrono::seconds(4), [&]() { return engine->sync_op_done; })) {
        return DSP_ERR_PORT_OPEN;
    }
    return engine->sync_op_result;
}

DSPCORE_API int32_t DSPCORE_CALL dsp_uso_refresh_diag(DspEngine* engine) {
    if (!engine) return DSP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(engine->mutex);
    if (!engine->running.load() || !engine->source) return DSP_ERR_NOT_FOUND;
    engine->pending_diag = true;
    return DSP_OK;
}

} /* extern "C" */

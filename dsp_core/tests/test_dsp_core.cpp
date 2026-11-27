#include "../include/dsp_core_api.h"
#include "../src/dsp_internal.hpp"
#include "../src/uso_proto_v2.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

namespace {

static int g_tests_passed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

/* Synthesize one or more modulated baseband frames on virtual grid (DLPOLr = 13)
 * and resample onto input grid spacing `input_step_h`. */
std::vector<double> synthesize_frames(
    int32_t ncod,
    int32_t frame_phase,
    const std::vector<std::vector<uint8_t>>& frames,
    double input_step_h,
    double amplitude,
    double noise_std,
    uint32_t seed,
    int32_t extra_virt_drift_per_frame = 0
) {
    std::vector<double> virt;
    const size_t lead_guard  = 160;
    const size_t trail_guard = 240;

    for (size_t i = 0; i < lead_guard; ++i) virt.push_back(0.0);

    const double p_sign = (frame_phase < 0) ? -1.0 : 1.0;

    for (size_t f = 0; f < frames.size(); ++f) {
        if (f > 0 && extra_virt_drift_per_frame > 0) {
            for (int d = 0; d < extra_virt_drift_per_frame; ++d) virt.push_back(0.0);
        }

        /* 128-chip Sync Preamble */
        for (int w = 0; w < 4; ++w) {
            uint32_t word = dsp::kGsp[static_cast<size_t>(ncod)][static_cast<size_t>(w)];
            for (int b = 0; b < 32; ++b) {
                double v = (((word >> (31 - b)) & 1u) ? 1.0 : -1.0) * p_sign * amplitude;
                for (int s = 0; s < DSP_DLPOL_R; ++s) virt.push_back(v);
            }
        }

        /* Data vectors (RM(1,4) + 5th bit phase inversion) */
        for (uint8_t code5 : frames[f]) {
            const uint32_t word = dsp::kGcodes[static_cast<size_t>(ncod)][code5 & 0x0Fu];
            const double inv = (code5 & 0x10u) ? -1.0 : 1.0;
            for (int b = 0; b < 32; ++b) {
                double v = (((word >> (31 - b)) & 1u) ? 1.0 : -1.0) * p_sign * inv * amplitude;
                for (int s = 0; s < DSP_DLPOL_R; ++s) virt.push_back(v);
            }
        }
    }

    for (size_t i = 0; i < trail_guard; ++i) virt.push_back(0.0);

    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, noise_std);

    const size_t out_len = static_cast<size_t>(std::floor(static_cast<double>(virt.size() - 1) * input_step_h));
    std::vector<double> input(out_len, 0.0);
    for (size_t k = 0; k < out_len; ++k) {
        const double tv = static_cast<double>(k) / input_step_h;
        const size_t i0 = static_cast<size_t>(std::floor(tv));
        const double a  = tv - static_cast<double>(i0);
        double val = virt[i0] + (virt[i0 + 1] - virt[i0]) * a;
        if (noise_std > 0.0) val += nd(rng);
        input[k] = val;
    }
    return input;
}

uint32_t local_crc32_ieee(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return ~crc;
}

/* Test 1: Legacy 24-bit (5-byte) & 32-bit (6-byte uco32) packet parser + 0xFE overflow */
void test_legacy_packet_parser() {
    int32_t sample = 0;

    const uint8_t pkt32_pos[6] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0xFF };
    CHECK(dsp_parse_legacy_packet(pkt32_pos, 6, 32, &sample) == DSP_OK);
    CHECK(sample == 0x01020304);

    const uint8_t pkt32_ovf[6] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFE, 0xFE };
    CHECK(dsp_parse_legacy_packet(pkt32_ovf, 6, 32, &sample) == DSP_ERR_OVERFLOW);
    CHECK(sample == -2);

    const uint8_t pkt24_neg[5] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(dsp_parse_legacy_packet(pkt24_neg, 5, 24, &sample) == DSP_OK);
    CHECK(sample == -1);

    const uint8_t pkt_bad[6] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0xAA };
    CHECK(dsp_parse_legacy_packet(pkt_bad, 6, 32, &sample) == DSP_ERR_INVALID_ARG);

    g_tests_passed++;
    std::printf("[PASS] test_legacy_packet_parser\n");
}

/* Test 2: Full-scale 32-bit ADC double-precision correlation (New USO 8 sps/bit, Ncod=1, inverted phase) */
void test_double_precision_32bit_adc_correlation() {
    DspConfig cfg{};
    dsp_config_default(&cfg);
    cfg.source_type     = DSP_SRC_NEW_ADC32; /* 8 sps/bit -> step H = 8/13 */
    cfg.carrier_freq_hz = 5.0;
    cfg.format_id       = DSP_FMT_STD_GK;
    cfg.ncod            = 1;
    cfg.dcp_threshold   = 70;

    struct Capture {
        int frames = 0;
        DspFrameData last{};
    } cap;

    DspCallbacks cbs{};
    cbs.user_data = &cap;
    cbs.on_frame = [](const DspFrameData* f, void* ud) {
        auto* c = static_cast<Capture*>(ud);
        c->frames++;
        c->last = *f;
    };

    DspEngine* eng = dsp_create(&cfg, &cbs);
    CHECK(eng != nullptr);

    std::vector<uint8_t> tx_vecs(39);
    for (size_t i = 0; i < 39; ++i) tx_vecs[i] = static_cast<uint8_t>(i % 32);

    const double step_h = dsp::compute_nominal_step_h(cfg.sample_rate_hz, cfg.carrier_freq_hz, cfg.source_type);
    auto sig = synthesize_frames(1, -1, {tx_vecs}, step_h, 1.2e9, 2.5e8, 42);

    CHECK(dsp_push_samples(eng, sig.data(), static_cast<int32_t>(sig.size()), 0) == DSP_OK);
    CHECK(cap.frames == 1);
    CHECK(cap.last.phase_polarity == -1);
    CHECK(cap.last.sync_corr_pct >= 85);

    for (size_t i = 0; i < 39; ++i) {
        CHECK(cap.last.vectors_5bit[i] == tx_vecs[i]);
        CHECK(cap.last.reliabilities[i] >= 80);
    }

    dsp_destroy(eng);
    g_tests_passed++;
    std::printf("[PASS] test_double_precision_32bit_adc_correlation (ISA=%s, Sync=%d%%)\n",
                dsp_get_simd_isa(), cap.last.sync_corr_pct);
}

/* Test 3: Zero-Latency OnToolfaceCallback fires BEFORE end of frame (TZ §3.4) */
void test_zero_latency_toolface_callback() {
    DspConfig cfg{};
    dsp_config_default(&cfg);
    cfg.source_type     = DSP_SRC_LEGACY_SERIAL;
    cfg.sample_rate_hz  = 250.0;
    cfg.carrier_freq_hz = 5.0;
    cfg.format_id       = DSP_FMT_STD_GK;
    cfg.ncod            = 0;

    struct EventLog {
        bool frame_finished = false;
        int  otk_events_before_frame_end = 0;
        double first_otk_grav = -1.0;
        double first_otk_mag  = -1.0;
        double first_otk_stat = -1.0;
    } log;

    DspCallbacks cbs{};
    cbs.user_data = &log;
    cbs.on_toolface = [](const DspToolfaceEvent* evt, void* ud) {
        auto* l = static_cast<EventLog*>(ud);
        if (!l->frame_finished) l->otk_events_before_frame_end++;
        if (evt->channel == DSP_TF_GRAV && l->first_otk_grav < 0.0) l->first_otk_grav = evt->angle_deg;
        if (evt->channel == DSP_TF_MAG  && l->first_otk_mag  < 0.0) l->first_otk_mag  = evt->angle_deg;
        if (evt->channel == DSP_TF_STAT && l->first_otk_stat < 0.0) l->first_otk_stat = evt->angle_deg;
    };
    cbs.on_frame = [](const DspFrameData*, void* ud) {
        static_cast<EventLog*>(ud)->frame_finished = true;
    };

    DspEngine* eng = dsp_create(&cfg, &cbs);
    CHECK(eng != nullptr);

    std::vector<uint8_t> vecs(39, 0);
    vecs[0]  = 0x00;
    vecs[1]  = 3;  vecs[2]  = 31; /* OTK1 = 127 * 360/254 = 180.0 deg */
    vecs[8]  = 1;  vecs[9]  = 31; /* AZ_OTK1 = 63 * 360/254 = 89.29 deg */
    vecs[10] = 9;  vecs[11] = 12; /* OTKu = 300 * 0.4 = 120.0 deg */
    vecs[3]  = 4;  vecs[4]  = 10; /* GK1 = 138 -> logarithmic GK */
    vecs[14] = 4;  vecs[15] = 10; /* GK2 */
    vecs[25] = 4;  vecs[26] = 10; /* GK3 */
    vecs[35] = 4;  vecs[36] = 10; /* GK4 */

    const double step_h = dsp::compute_nominal_step_h(250.0, 5.0, DSP_SRC_LEGACY_SERIAL);
    auto sig = synthesize_frames(0, 1, {vecs}, step_h, 50000.0, 2000.0, 7);

    /* Push incrementally in 64-sample chunks */
    for (size_t pos = 0; pos < sig.size(); pos += 64) {
        int32_t n = static_cast<int32_t>(std::min<size_t>(64, sig.size() - pos));
        CHECK(dsp_push_samples(eng, sig.data() + pos, n, 0) == DSP_OK);
    }

    DspFrameData final_frame{};
    CHECK(dsp_get_last_frame(eng, &final_frame) == DSP_OK);

    CHECK(log.frame_finished);
    CHECK(log.otk_events_before_frame_end == 9);
    CHECK(std::fabs(log.first_otk_grav - 180.0) < 0.1);
    CHECK(std::fabs(log.first_otk_mag  - (63.0 * 360.0 / 254.0)) < 0.1);
    CHECK(std::fabs(log.first_otk_stat - 120.0) < 0.1);
    CHECK(final_frame.gk_zts > 0.0);

    dsp_destroy(eng);
    g_tests_passed++;
    std::printf("[PASS] test_zero_latency_toolface_callback (9 OTK events before frame end; OTK1=%.1f°)\n",
                log.first_otk_grav);
}

/* Test 4: Real legacy FIR filter ../KIH5_4F.dat (3888 taps) loaded via C ABI */
void test_real_legacy_kih_fir_file() {
    DspConfig cfg{};
    dsp_config_default(&cfg);

    double first_filtered = 0.0;
    DspCallbacks cbs{};
    cbs.user_data = &first_filtered;
    cbs.on_signal = [](const DspSignalBlock* blk, void* ud) {
        if (blk->sample_count > 0) {
            *static_cast<double*>(ud) = blk->filtered_samples[0];
        }
    };

    DspEngine* eng = dsp_create(&cfg, &cbs);
    CHECK(eng != nullptr);
    const int32_t taps = dsp_load_fir_file(eng, "../KIH5_4F.dat");
    CHECK(taps == 3887);

    std::vector<double> impulse(32, 0.0);
    impulse[0] = 1.0;
    CHECK(dsp_push_samples(eng, impulse.data(), 32, 0) == DSP_OK);
    CHECK(std::fabs(first_filtered - 8.07174957428991e-09) < 1e-18);

    dsp_destroy(eng);
    g_tests_passed++;
    std::printf("[PASS] test_real_legacy_kih_fir_file (%d taps verified from ../KIH5_4F.dat)\n", taps);
}

/* Test 5: Dynamic step H quartz drift tracking (PIK5 / ID) across 7 frames via C ABI */
void test_pik5_dynamic_step_h_tracking() {
    DspConfig cfg{};
    dsp_config_default(&cfg);
    cfg.source_type       = DSP_SRC_LEGACY_SERIAL;
    cfg.sample_rate_hz    = 250.0;
    cfg.carrier_freq_hz   = 5.0;
    cfg.format_id         = DSP_FMT_STD; /* 33 vectors */
    cfg.enable_step_track = 1;
    cfg.dcp_threshold     = 65;

    int frame_cnt = 0;
    DspCallbacks cbs{};
    cbs.user_data = &frame_cnt;
    cbs.on_frame = [](const DspFrameData*, void* ud) {
        (*static_cast<int*>(ud))++;
    };

    DspEngine* eng = dsp_create(&cfg, &cbs);
    CHECK(eng != nullptr);
    const double initial_h = dsp_get_step_h(eng);

    /* Transmit 7 consecutive frames with +10 virtual samples extra spacing per frame (crystal drift) */
    std::vector<std::vector<uint8_t>> frames(7, std::vector<uint8_t>(33, 5));
    auto sig = synthesize_frames(0, 1, frames, initial_h, 40000.0, 500.0, 99, 10);

    CHECK(dsp_push_samples(eng, sig.data(), static_cast<int32_t>(sig.size()), 0) == DSP_OK);
    const double tracked_h = dsp_get_step_h(eng);

    CHECK(frame_cnt >= 6);
    CHECK(tracked_h > initial_h);

    dsp_destroy(eng);
    g_tests_passed++;
    std::printf("[PASS] test_pik5_dynamic_step_h_tracking (%d frames; H: %.6f -> %.6f)\n",
                frame_cnt, initial_h, tracked_h);
}

/* Test 6: Format 30 Geomash packet packing + CRC32 verification end-to-end */
void test_geomash_crc32_format30() {
    DspConfig cfg{};
    dsp_config_default(&cfg);
    cfg.source_type     = DSP_SRC_NEW_ADC32;
    cfg.carrier_freq_hz = 5.0;
    cfg.format_id       = DSP_FMT_GEOMASH; /* 34 vectors: 8 header + 26 payload (16 bytes) */

    DspFrameData out{};
    int frames = 0;
    struct Ctx { DspFrameData* out; int* frames; } ctx{ &out, &frames };

    DspCallbacks cbs{};
    cbs.user_data = &ctx;
    cbs.on_frame = [](const DspFrameData* f, void* ud) {
        auto* c = static_cast<Ctx*>(ud);
        *(c->out) = *f;
        (*(c->frames))++;
    };

    DspEngine* eng = dsp_create(&cfg, &cbs);
    CHECK(eng != nullptr);

    /* Build 16-byte Geomash payload: 12 data bytes + 4-byte little-endian CRC32 */
    uint8_t payload[16]{};
    for (int i = 0; i < 12; ++i) payload[i] = static_cast<uint8_t>(0x10 + i * 3);
    const uint32_t crc = local_crc32_ieee(payload, 12);
    payload[12] = static_cast<uint8_t>(crc & 0xFFu);
    payload[13] = static_cast<uint8_t>((crc >> 8) & 0xFFu);
    payload[14] = static_cast<uint8_t>((crc >> 16) & 0xFFu);
    payload[15] = static_cast<uint8_t>((crc >> 24) & 0xFFu);

    /* Split 16 bytes (128 bits) into 26 x 5-bit symbols (130 bits, 2 padding bits) */
    std::vector<uint8_t> vecs(34, 0);
    vecs[0] = 0x10; /* Static mode */
    vecs[4] = 0x08; /* QUAL_STAT=1 */
    vecs[5] = 15;   /* VIBR_L */
    uint32_t bitbuf = 0;
    int bits = 0;
    size_t byte_i = 0;
    for (size_t v = 0; v < 26; ++v) {
        while (bits < 5) {
            uint8_t next_b = (byte_i < 16) ? payload[byte_i++] : 0;
            bitbuf = (bitbuf << 8) | next_b;
            bits += 8;
        }
        vecs[8 + v] = static_cast<uint8_t>((bitbuf >> (bits - 5)) & 0x1Fu);
        bits -= 5;
        bitbuf &= (1u << bits) - 1u;
    }

    const double step_h = dsp::compute_nominal_step_h(cfg.sample_rate_hz, cfg.carrier_freq_hz, cfg.source_type);
    auto sig = synthesize_frames(0, 1, {vecs}, step_h, 100000.0, 1000.0, 123);

    CHECK(dsp_push_samples(eng, sig.data(), static_cast<int32_t>(sig.size()), 0) == DSP_OK);
    CHECK(frames == 1);
    CHECK(out.crc_valid == 1);
    CHECK(out.qual_stat == 1);
    CHECK(out.vibr_zts == 15);

    dsp_destroy(eng);
    g_tests_passed++;
    std::printf("[PASS] test_geomash_crc32_format30 (CRC32=0x%08X verified)\n", crc);
}

/* Test 7: Real legacy .smp binary recording playback via SmpFilePlayerSource + C ABI */
void test_real_smp_file_player() {
    const char* candidates[] = {
        "C:\\Projects\\AI\\TelesystemAiStudio\\dsp_core\\out\\build\\26-06-26 09-23-41.smp",
        "../tek/smp/26-06-26 15-44-56.smp",
        "tek/smp/26-06-26 15-44-56.smp"
    };
    const char* smp_path = nullptr;
    for (const char* p : candidates) {
        if (FILE* f = std::fopen(p, "rb")) {
            std::fclose(f);
            smp_path = p;
            break;
        }
    }
    CHECK(smp_path != nullptr);

    DspConfig cfg{};
    dsp_config_default(&cfg);
    cfg.source_type    = DSP_SRC_FILE_PLAYER;
    cfg.uso_bit_mode   = 32;
    cfg.playback_speed = 0;
    std::snprintf(cfg.port_or_path, sizeof(cfg.port_or_path), "%s", smp_path);

    struct SmpStats {
        int64_t total_samples = 0;
        int     blocks = 0;
        double  first_sample = 0.0;
    } st;

    DspCallbacks cbs{};
    cbs.user_data = &st;
    cbs.on_signal = [](const DspSignalBlock* blk, void* ud) {
        auto* s = static_cast<SmpStats*>(ud);
        if (s->blocks == 0 && blk->sample_count > 0 && blk->raw_samples) {
            s->first_sample = blk->raw_samples[0];
        }
        s->total_samples += blk->sample_count;
        s->blocks++;
    };

    DspEngine* eng = dsp_create(&cfg, &cbs);
    CHECK(eng != nullptr);
    CHECK(dsp_start(eng) == DSP_OK);

    for (int i = 0; i < 60 && dsp_is_running(eng) != 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    dsp_stop(eng);
    CHECK(dsp_is_running(eng) == 0);
    dsp_destroy(eng);

    CHECK(st.blocks > 0);
    CHECK(st.total_samples > 0);
    /* Verify 2048-byte ASCII "----" header (0x2D2D2D2D = 757935405) was skipped */
    CHECK(std::fabs(st.first_sample - 757935405.0) > 1.0);

    g_tests_passed++;
    std::printf("[PASS] test_real_smp_file_player (%lld samples streamed from legacy .smp, header skipped)\n",
                static_cast<long long>(st.total_samples));
}

/* Test 8: USO-32 Dual-Mode Protocol V2 framing, CRC-16, batching & C ABI parity */
void test_uso_v2_protocol_and_abi() {
    /* Exact C ABI sizes matching ProvodkaNG.Core DspNativeTypes.cs */
    static_assert(sizeof(DspUsoV2Options) == 56, "DspUsoV2Options ABI size mismatch");
    static_assert(sizeof(DspUsoRuntimeInfo) == 96, "DspUsoRuntimeInfo ABI size mismatch");

    /* Batch encode + incremental parse round-trip */
    dsp::usov2::DataBatch tx{};
    tx.timestamp_ms     = 55123u;
    tx.first_sample_idx = 4096u;
    tx.effective_fs_dhz = 2000u; /* 200.0 Hz */
    tx.flags            = dsp::usov2::kBatchPgaOvf | dsp::usov2::kBatchHpfActive;
    tx.pga_gain         = 4u;
    tx.sample_count     = 8u;
    for (uint8_t i = 0; i < tx.sample_count; ++i) {
        tx.samples[i] = (i % 2 == 0) ? (250000 * static_cast<int32_t>(i + 1)) : (-125000 * static_cast<int32_t>(i + 1));
    }

    uint8_t wire[dsp::usov2::kMaxFrame]{};
    unsigned wlen = dsp::usov2::encode_batch_for_test(77u, tx, wire);
    CHECK(wlen == 9u + 12u + 32u); /* 53 bytes < 64 USB FS MPS */

    dsp::usov2::StreamParser parser;
    dsp::usov2::Frame f{};
    int r = 0;
    /* Feed leading garbage followed by valid V2 batch frame */
    CHECK(parser.feed(0x00, f) == 0);
    CHECK(parser.feed(0xFF, f) == 0);
    for (unsigned i = 0; i < wlen; ++i) {
        r = parser.feed(wire[i], f);
        if (i + 1 < wlen) CHECK(r == 0);
    }
    CHECK(r == 1);
    CHECK(f.msg_type == dsp::usov2::MSG_DATA_BATCH);
    CHECK(f.seq == 77u);

    dsp::usov2::DataBatch rx{};
    CHECK(dsp::usov2::unpack_batch(f.payload, f.payload_len, rx));
    CHECK(rx.timestamp_ms == tx.timestamp_ms);
    CHECK(rx.first_sample_idx == tx.first_sample_idx);
    CHECK(rx.effective_fs_dhz == 2000u);
    CHECK(rx.flags == tx.flags);
    CHECK(rx.pga_gain == 4u);
    CHECK(rx.sample_count == 8u);
    for (uint8_t i = 0; i < 8u; ++i) CHECK(rx.samples[i] == tx.samples[i]);

    /* Verify corrupted CRC is rejected */
    wire[12] ^= 0x80u;
    for (unsigned i = 0; i < wlen; ++i) r = parser.feed(wire[i], f);
    CHECK(r == -1);
    CHECK(parser.crc_errors() == 1u);

    /* Verify C ABI options & runtime info calls */
    DspUsoV2Options opts{};
    dsp_uso_v2_options_default(&opts);
    CHECK(opts.adaptive_rate == 1);
    CHECK(opts.adc_filter == dsp::usov2::FLT_SINC4);
    CHECK(opts.mcu_lpf_mode == dsp::usov2::LPF_BYPASS);

    DspConfig cfg{};
    dsp_config_default(&cfg);
    DspEngine* eng = dsp_create(&cfg, nullptr);
    CHECK(eng != nullptr);
    CHECK(dsp_set_uso_v2_options(eng, &opts) == DSP_OK);
    DspUsoRuntimeInfo info{};
    CHECK(dsp_get_uso_info(eng, &info) == DSP_OK);
    CHECK(info.protocol_version == 0);
    CHECK(std::fabs(info.active_sample_rate_hz - 250.0) < 1e-6);
    dsp_destroy(eng);

    g_tests_passed++;
    std::printf("[PASS] test_uso_v2_protocol_and_abi (V2Options=56B, RuntimeInfo=96B, CRC16 batch OK)\n");
}

} // namespace

int main() {
    std::printf("=== DspCore Native C++17 Verification Suite (ISA: %s) ===\n", dsp_get_simd_isa());
    test_legacy_packet_parser();
    test_double_precision_32bit_adc_correlation();
    test_zero_latency_toolface_callback();
    test_real_legacy_kih_fir_file();
    test_pik5_dynamic_step_h_tracking();
    test_geomash_crc32_format30();
    test_real_smp_file_player();
    test_uso_v2_protocol_and_abi();
    std::printf("=== ALL %d TESTS PASSED ===\n", g_tests_passed);
    return 0;
}

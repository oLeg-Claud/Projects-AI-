#include "frame_decoder.hpp"
#include "dsp_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace dsp {

namespace {
constexpr double kPi      = 3.14159265358979323846;
constexpr double kDeg2Rad = kPi / 180.0;
constexpr double kRad2Deg = 180.0 / kPi;
constexpr double kScaleOtk = 360.0 / 254.0;
constexpr double kScaleZu  = 10240.0 / 4096.0;

inline double angular_delta(double a, double b) {
    double d = std::fmod(a - b + 540.0, 360.0) - 180.0;
    return std::fabs(d);
}

inline int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}
} // namespace

/* ============================================================================
 * Legacy Bit-Level & Math Utilities (ported verbatim from KORDAT2k.cpp)
 * ============================================================================ */
void pars_bits(uint8_t src, int8_t begist, int8_t endist, uint16_t* ptrnazn, int8_t begnazn) {
    static constexpr uint16_t kMasks[6] = { 0, 0x0001u, 0x0003u, 0x0007u, 0x000Fu, 0x001Fu };
    const int8_t dlina = static_cast<int8_t>(1 + begist - endist);
    if (!ptrnazn || dlina > 5 || dlina <= 0 || begnazn > 15) return;

    const int8_t dvig = static_cast<int8_t>(begist - begnazn);
    uint16_t res = 0;
    if (dvig >= 0) {
        res = static_cast<uint16_t>((src & (kMasks[dlina] << endist)) >> dvig);
    } else {
        res = static_cast<uint16_t>((src & (kMasks[dlina] << endist)) << (-dvig));
    }
    const uint16_t clear_mask = static_cast<uint16_t>(~(kMasks[dlina] << (1 + begnazn - dlina)));
    *ptrnazn = static_cast<uint16_t>((*ptrnazn & clear_mask) | res);
}

uint32_t join_5bit_to_bytes(const uint8_t* in, uint32_t len, uint8_t* out) {
    if (!in || !out || len == 0) return 0;
    uint32_t buf = 0;
    uint8_t bits = 0;
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < len; ++i) {
        if (in[i] == 255u) return 0;
        buf = (buf << 5) | (in[i] & 0x1Fu);
        bits = static_cast<uint8_t>(bits + 5);
        while (bits >= 8) {
            out[cnt++] = static_cast<uint8_t>(buf >> (bits - 8));
            bits = static_cast<uint8_t>(bits - 8);
            buf &= (1u << bits) - 1u;
        }
    }
    return cnt;
}

uint32_t compute_crc32_ieee(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            if (crc & 1u) crc = (crc >> 1) ^ 0xEDB88320u;
            else crc >>= 1;
        }
    }
    return ~crc;
}

double scale_gk_log(uint8_t code, double kgk_zts) {
    if (code == 0) return 0.0;
    constexpr double K1 = 1.5437;
    constexpr double K3 = 2.1957;
    constexpr double SF = 256.0;
    return ((std::pow(10.0, (K3 * static_cast<double>(code)) / (SF - 1.0)) - 1.0) / K1) * kgk_zts;
}

double rotor_deg(double up, double down) {
    double ang = std::atan2(up, down) * kRad2Deg;
    if (ang < 0.0) ang += 360.0;
    return std::fmod(ang, 360.0);
}

double resolve_az_deg(double hx, double hy, double hz, double rzu, double rotk) {
    const double up   = -hx * std::sin(rotk) - hy * std::cos(rotk);
    const double down = hz * std::sin(rzu) + std::cos(rzu) * (hx * std::cos(rotk) - hy * std::sin(rotk));
    return rotor_deg(up, down);
}

/* ============================================================================
 * FrameDecoder Implementation
 * ============================================================================ */
FrameDecoder::FrameDecoder()
    : hx_raw_(0),
      hy_raw_(0),
      hz_raw_(0),
      servis_raw_(0),
      geomash_count_(0),
      pause_vecs_per_slot_(0),
      base_vectors_(39) {
    std::memset(&cfg_, 0, sizeof(cfg_));
    std::memset(&frame_, 0, sizeof(frame_));
    dig_.fill(0);
    rel_.fill(0);
}

void FrameDecoder::configure(const DspConfig& cfg) {
    cfg_ = cfg;
    const FormatGeometry g = get_format_geometry(cfg_.format_id);
    base_vectors_        = g.base_vectors;
    // Format 13 uses one vector per repeater pause, including at 16 Hz.
    // Its 37 slots already include pauses at logical indices 1, 11, 21, 30.
    pause_vecs_per_slot_ = cfg_.format_id == DSP_FMT_STD_RETR ? 1
        : (g.pause_slots > 0 ? get_pause_vectors_per_slot(cfg_.carrier_freq_hz) : 0);
}

void FrameDecoder::begin_frame(
    int32_t frame_number,
    int32_t phase_polarity,
    int32_t sync_corr_pct,
    double step_h,
    int32_t last_pik_drift
) {
    /* Preserve previous physical readings across frames, reset per-frame headers/vectors */
    frame_.frame_number   = frame_number;
    frame_.format_id      = cfg_.format_id;
    frame_.phase_polarity = phase_polarity;
    frame_.sync_corr_pct  = sync_corr_pct;
    frame_.step_h         = step_h;
    frame_.last_pik_drift = last_pik_drift;
    frame_.crc_valid      = -1;
    frame_.vector_count   = 0;

    dig_.fill(0);
    rel_.fill(0);
    std::memset(frame_.vectors_5bit, 0, sizeof(frame_.vectors_5bit));
    std::memset(frame_.reliabilities, 0, sizeof(frame_.reliabilities));
}

int32_t FrameDecoder::total_physical_vectors() const {
    const FormatGeometry g = get_format_geometry(cfg_.format_id);
    if (g.pause_slots <= 0 || pause_vecs_per_slot_ <= 1) {
        return g.base_vectors;
    }
    /* Each pause slot in format*_2k.h already occupies 1 index in base_vectors;
     * when carrier frequency requires >1 pause vector per slot, add (pause_vecs_per_slot_ - 1) extra. */
    return g.base_vectors + g.pause_slots * (pause_vecs_per_slot_ - 1);
}

namespace {
/* Exact pause logical indices from format2_2k.h, format11_2k.h, format13_2k.h, format31_2k.h */
const int32_t* get_pause_positions(int32_t format_id, int32_t* out_count) {
    static constexpr int32_t kPausesR2[7]  = { 1, 17, 33, 48, 63, 78, 93 };
    static constexpr int32_t kPausesR11[4] = { 1, 13, 25, 36 };
    static constexpr int32_t kPausesR13[4] = { 1, 11, 21, 30 };
    static constexpr int32_t kPausesR31[1] = { 34 };

    switch (format_id) {
        case DSP_FMT_NDM_GK_RETR:  *out_count = 7; return kPausesR2;
        case DSP_FMT_GK_RETR:      *out_count = 4; return kPausesR11;
        case DSP_FMT_STD_RETR:     *out_count = 4; return kPausesR13;
        case DSP_FMT_GEOMASH_RETR: *out_count = 1; return kPausesR31;
        default:                   *out_count = 0; return nullptr;
    }
}
} // namespace

int32_t FrameDecoder::physical_to_logical_kod(int32_t phys_slot) const {
    int32_t pcount = 0;
    const int32_t* pauses = get_pause_positions(cfg_.format_id, &pcount);
    if (pcount == 0 || pause_vecs_per_slot_ <= 0) {
        return (phys_slot >= 0 && phys_slot < base_vectors_) ? phys_slot : -1;
    }

    int32_t cur_phys = 0;
    for (int32_t logical = 0; logical < base_vectors_; ++logical) {
        bool is_pause = false;
        for (int32_t p = 0; p < pcount; ++p) {
            if (pauses[p] == logical) {
                is_pause = true;
                break;
            }
        }
        const int32_t span = is_pause ? std::max<int32_t>(1, pause_vecs_per_slot_) : 1;
        if (phys_slot >= cur_phys && phys_slot < cur_phys + span) {
            return is_pause ? -1 : logical;
        }
        cur_phys += span;
    }
    return -1;
}

bool FrameDecoder::is_pause_slot(int32_t phys_slot) const {
    return physical_to_logical_kod(phys_slot) < 0;
}

void FrameDecoder::emit_toolface(
    DspToolfaceChannel ch,
    int32_t vec_idx,
    double angle_deg,
    int32_t rel_pct,
    double latency_ms,
    const ToolfaceSink& sink
) {
    if (angle_deg < 0.0 || angle_deg > 360.0) return;
    if (ch == DSP_TF_GRAV)      frame_.otk_grav_deg = angle_deg;
    else if (ch == DSP_TF_MAG)  frame_.otk_mag_deg  = angle_deg;
    else if (ch == DSP_TF_STAT) frame_.otk_stat_deg = angle_deg;

    if (sink) {
        DspToolfaceEvent evt{};
        evt.timestamp_us    = now_us();
        evt.channel         = static_cast<int32_t>(ch);
        evt.vector_index    = vec_idx;
        evt.angle_deg       = angle_deg;
        evt.reliability_pct = rel_pct;
        evt.latency_ms      = latency_ms;
        sink(evt);
    }
}

void FrameDecoder::compute_surface_inclinometry(int32_t hx_raw, int32_t hy_raw, int32_t hz_raw) {
    const int32_t hx = hx_raw * 40;
    const int32_t hy = hy_raw * 40;
    const int32_t hz = hz_raw * 40;

    frame_.hx      = hx;
    frame_.hy      = hy;
    frame_.hz      = hz;
    frame_.h_total = std::sqrt(
        static_cast<double>(hx) * hx +
        static_cast<double>(hy) * hy +
        static_cast<double>(hz) * hz
    );

    const double rzu  = frame_.zu_deg * kDeg2Rad;
    const double rotk = frame_.otk_stat_deg * kDeg2Rad;
    const double gmod = (frame_.g_total > 0.1) ? frame_.g_total : 1.0;

    const double gx_u = -std::sin(rzu) * std::cos(rotk);
    const double gy_u =  std::sin(rzu) * std::sin(rotk);
    const double gz_u =  std::cos(rzu);

    frame_.gx = gx_u * gmod;
    frame_.gy = gy_u * gmod;
    frame_.gz = gz_u * gmod;

    if (frame_.h_total > 1e-6) {
        const double dot_gh = (hx * gx_u + hy * gy_u + hz * gz_u) / frame_.h_total;
        const double clamped = std::max(-1.0, std::min(1.0, dot_gh));
        frame_.dip_calc_deg = std::asin(clamped) * kRad2Deg;
    }

    /* Raw magnetic azimuth (KORDAT2k.cpp lines 893-895) */
    const double up   = -hx * gy_u + hy * gx_u;
    const double down = hz - gz_u * (hx * gx_u + hy * gy_u + hz * gz_u);
    frame_.az_raw_deg = rotor_deg(up, down);

    /* Corrected azimuth: Standard vs NUBT short-collar (KORDAT2k.cpp lines 898-931) */
    const double H   = (cfg_.ref_hop_nt > 100.0) ? cfg_.ref_hop_nt : 46500.0;
    const double mnr = (90.0 - cfg_.ref_dip_deg) * kDeg2Rad;
    double az_korr   = -9999.0;

    if (!cfg_.use_nubt && std::fabs(std::sin(mnr)) > 1e-6) {
        const double n1 = (hx + std::sin(rzu) * std::cos(rotk) * std::cos(mnr) * H) / std::sin(mnr);
        const double n2 = (hy - std::sin(rzu) * std::sin(rotk) * std::cos(mnr) * H) / std::sin(mnr);
        const double k1 = std::cos(rzu) * std::cos(rotk);
        const double k2 = std::sin(rotk);
        const double k3 = std::cos(rzu) * std::sin(rotk);
        const double k4 = std::cos(rotk);
        const double denom = k1 * k4 + k2 * k3;
        if (std::fabs(denom) > 1e-9) {
            const double up1   = (n1 * k3 + n2 * k1) / -denom;
            const double down1 = (n1 * k4 - n2 * k2) / denom;
            az_korr = rotor_deg(up1, down1);
        }
    } else if (cfg_.use_nubt) {
        const double mod = H * H - (static_cast<double>(hx) * hx + static_cast<double>(hy) * hy);
        if (mod >= 0.0) {
            const double hz_est = std::sqrt(mod);
            const double az1 = resolve_az_deg(hx, hy,  hz_est, rzu, rotk);
            const double az2 = resolve_az_deg(hx, hy, -hz_est, rzu, rotk);
            az_korr = (angular_delta(cfg_.ref_az_op_deg, az1) <= angular_delta(cfg_.ref_az_op_deg, az2))
                          ? az1
                          : az2;
        }
    }

    frame_.az_korr_deg = az_korr;
    if (az_korr != -9999.0) {
        frame_.az_true_deg = std::fmod(az_korr + cfg_.ref_dec_deg + 360.0, 360.0);
    } else {
        frame_.az_true_deg = -9999.0;
    }
}

void FrameDecoder::apply_service_param() {
    const int32_t t = servis_raw_;
    switch (frame_.service_mode) {
        case 0: frame_.rpm_zts       = t * 20; break;
        case 1: frame_.temp_zts      = static_cast<double>(static_cast<int8_t>(t & 0xFF)); break;
        case 2: frame_.volt_zts      = static_cast<double>(t) / 10.0; break;
        case 7: frame_.mah_ndm       = static_cast<double>(t) / 10.0; break;
        case 8: frame_.kks_signal_mv = static_cast<double>(t) / 10.0; break;
        case 9: frame_.battery_pct   = t; break;
        default: break;
    }
}

void FrameDecoder::process_logical_vector(
    int32_t kod,
    uint8_t code_5bit,
    int32_t reliability_pct,
    double decode_latency_ms,
    const ToolfaceSink& toolface_sink
) {
    if (kod < 0 || kod >= DSP_MAX_VECTORS) return;

    dig_[static_cast<size_t>(kod)] = static_cast<uint8_t>(code_5bit & 0x1Fu);
    rel_[static_cast<size_t>(kod)] = static_cast<uint8_t>(std::max(0, std::min(100, reliability_pct)));
    frame_.vectors_5bit[kod]       = dig_[static_cast<size_t>(kod)];
    frame_.reliabilities[kod]      = rel_[static_cast<size_t>(kod)];
    if (kod + 1 > frame_.vector_count) {
        frame_.vector_count = kod + 1;
    }

    const double kgk = (cfg_.kgk_zts > 0.0) ? cfg_.kgk_zts : 1.0;

    /* Vector 0 is always FIRST_KOD (ctat + iot) across all 7 formats */
    if (kod == 0 && reliability_pct >= cfg_.dcp_threshold) {
        frame_.is_static_mode = (dig_[0] >> 4) & 0x01;
        frame_.service_mode   = dig_[0] & 0x0F;
        return;
    }

    auto decode_otk8 = [&](int32_t hi, int32_t lo) -> double {
        const int32_t raw8 = ((dig_[static_cast<size_t>(hi)] & 0x07) * 32) + dig_[static_cast<size_t>(lo)];
        return static_cast<double>(raw8) * kScaleOtk;
    };

    auto decode_gk8 = [&](int32_t hi, int32_t lo) -> double {
        const uint8_t raw8 = static_cast<uint8_t>(((dig_[static_cast<size_t>(hi)] & 0x07) * 32) + dig_[static_cast<size_t>(lo)]);
        return scale_gk_log(raw8, kgk);
    };

    auto sign_extend_12 = [](uint16_t v) -> int32_t {
        if (v & 0x0800u) v = static_cast<uint16_t>(v | 0xF000u);
        return static_cast<int16_t>(v);
    };

    /* ========================================================================
     * Format №1 (FORMAT_STD_GK) & Format №11 (FORMAT_GK_RETR)
     * Exact indices from format1_2k.h & format11_2k.h
     * ======================================================================== */
    if (cfg_.format_id == DSP_FMT_STD_GK || cfg_.format_id == DSP_FMT_GK_RETR) {
        const bool r1 = (cfg_.format_id == DSP_FMT_STD_GK);
        const int32_t OTK1_H = r1 ? 1 : 2,   OTK1_L = r1 ? 2 : 3;
        const int32_t GK1_H  = r1 ? 3 : 4,   GK1_L  = r1 ? 4 : 5;
        const int32_t ZU_H   = r1 ? 5 : 6,   ZU_L   = r1 ? 6 : 7;
        const int32_t GMOD_H = r1 ? 7 : 8;
        const int32_t AZ1_H  = r1 ? 8 : 9,   AZ1_L  = r1 ? 9 : 10;
        const int32_t OTKu_H = r1 ? 10 : 11, OTKu_L = r1 ? 11 : 12;

        const int32_t OTK2_H = r1 ? 12 : 14, OTK2_L = r1 ? 13 : 15;
        const int32_t GK2_H  = r1 ? 14 : 16, GK2_L  = r1 ? 15 : 17;
        const int32_t GMOD_L = r1 ? 16 : 18;
        const int32_t HX_H   = r1 ? 17 : 19, HX_M   = r1 ? 18 : 20;
        const int32_t AZ2_H  = r1 ? 19 : 21, AZ2_L  = r1 ? 20 : 22;
        const int32_t HX_L   = r1 ? 21 : 23, HY_H   = r1 ? 22 : 24;

        const int32_t OTK3_H = r1 ? 23 : 26, OTK3_L = r1 ? 24 : 27;
        const int32_t GK3_H  = r1 ? 25 : 28, GK3_L  = r1 ? 26 : 29;
        const int32_t HY_M   = r1 ? 27 : 30, HY_L   = r1 ? 28 : 31;
        const int32_t AZ3_H  = r1 ? 29 : 32, AZ3_L  = r1 ? 30 : 33;
        const int32_t HZ_H   = r1 ? 31 : 34, HZ_L   = r1 ? 32 : 35;

        const int32_t OTK4_H = r1 ? 33 : 37, OTK4_L = r1 ? 34 : 38;
        const int32_t GK4_H  = r1 ? 35 : 39, GK4_L  = r1 ? 36 : 40;
        const int32_t AZ4_H  = r1 ? 37 : 41, AZ4_L  = r1 ? 38 : 42;

        if (kod == OTK1_L)      emit_toolface(DSP_TF_GRAV, kod, decode_otk8(OTK1_H, OTK1_L), reliability_pct, decode_latency_ms, toolface_sink);
        else if (kod == OTK2_L) emit_toolface(DSP_TF_GRAV, kod, decode_otk8(OTK2_H, OTK2_L), reliability_pct, decode_latency_ms, toolface_sink);
        else if (kod == OTK3_L) emit_toolface(DSP_TF_GRAV, kod, decode_otk8(OTK3_H, OTK3_L), reliability_pct, decode_latency_ms, toolface_sink);
        else if (kod == OTK4_L) emit_toolface(DSP_TF_GRAV, kod, decode_otk8(OTK4_H, OTK4_L), reliability_pct, decode_latency_ms, toolface_sink);
        else if (kod == AZ1_L)  emit_toolface(DSP_TF_MAG,  kod, decode_otk8(AZ1_H, AZ1_L),   reliability_pct, decode_latency_ms, toolface_sink);
        else if (kod == AZ2_L)  emit_toolface(DSP_TF_MAG,  kod, decode_otk8(AZ2_H, AZ2_L),   reliability_pct, decode_latency_ms, toolface_sink);
        else if (kod == AZ3_L)  emit_toolface(DSP_TF_MAG,  kod, decode_otk8(AZ3_H, AZ3_L),   reliability_pct, decode_latency_ms, toolface_sink);
        else if (kod == AZ4_L)  emit_toolface(DSP_TF_MAG,  kod, decode_otk8(AZ4_H, AZ4_L),   reliability_pct, decode_latency_ms, toolface_sink);
        else if (kod == OTKu_L) {
            const double ang = static_cast<double>(dig_[static_cast<size_t>(OTKu_H)] * 32 + dig_[static_cast<size_t>(OTKu_L)]) * 0.4;
            emit_toolface(DSP_TF_STAT, kod, ang, reliability_pct, decode_latency_ms, toolface_sink);
        }
        else if (kod == GK1_L)  frame_.gk_zts = decode_gk8(GK1_H, GK1_L);
        else if (kod == GK2_L)  frame_.gk_zts = decode_gk8(GK2_H, GK2_L);
        else if (kod == GK3_L)  frame_.gk_zts = decode_gk8(GK3_H, GK3_L);
        else if (kod == GK4_L)  frame_.gk_zts = decode_gk8(GK4_H, GK4_L);
        else if (kod == GMOD_L) {
            int16_t t = static_cast<int16_t>(dig_[static_cast<size_t>(GMOD_H)] * 32 + dig_[static_cast<size_t>(GMOD_L)]);
            t = static_cast<int16_t>(t << 6) >> 6;
            frame_.g_total = 1.0 + static_cast<double>(t) / 4096.0;
        }
        else if (kod == HY_H) {
            uint16_t t = 0;
            pars_bits(dig_[static_cast<size_t>(HX_H)], 0, 0, &t, 11);
            pars_bits(dig_[static_cast<size_t>(HX_M)], 4, 0, &t, 10);
            pars_bits(dig_[static_cast<size_t>(HX_L)], 4, 0, &t, 5);
            pars_bits(dig_[static_cast<size_t>(HY_H)], 4, 4, &t, 0);
            hx_raw_ = sign_extend_12(t);
        }
        else if (kod == HY_L) {
            uint16_t t = 0;
            pars_bits(dig_[static_cast<size_t>(HY_H)], 3, 0, &t, 11);
            pars_bits(dig_[static_cast<size_t>(HY_M)], 4, 0, &t, 7);
            pars_bits(dig_[static_cast<size_t>(HY_L)], 4, 2, &t, 2);
            hy_raw_ = sign_extend_12(t);
        }
        else if (kod == HZ_L) {
            uint16_t t = 0;
            pars_bits(dig_[static_cast<size_t>(HY_L)], 1, 0, &t, 11);
            pars_bits(dig_[static_cast<size_t>(HZ_H)], 4, 0, &t, 9);
            pars_bits(dig_[static_cast<size_t>(HZ_L)], 4, 0, &t, 4);
            hz_raw_ = sign_extend_12(t);
        }
        else if (kod == OTK3_H) {
            uint16_t t = 0;
            pars_bits(dig_[static_cast<size_t>(OTK1_H)], 4, 3, &t, 9);
            pars_bits(dig_[static_cast<size_t>(GK1_H)],  4, 3, &t, 7);
            pars_bits(dig_[static_cast<size_t>(AZ1_H)],  4, 3, &t, 5);
            pars_bits(dig_[static_cast<size_t>(OTK2_H)], 4, 3, &t, 3);
            pars_bits(dig_[static_cast<size_t>(GK2_H)],  4, 3, &t, 1);
            servis_raw_ = t;
            apply_service_param();
        }
        else if (kod == GK4_H) {
            uint16_t t = 0;
            pars_bits(dig_[static_cast<size_t>(AZ3_H)],  4, 3, &t, 5);
            pars_bits(dig_[static_cast<size_t>(OTK4_H)], 4, 3, &t, 3);
            pars_bits(dig_[static_cast<size_t>(GK4_H)],  4, 3, &t, 1);
            frame_.vibr_zts = t;
        }
        else if (kod == AZ4_H) {
            const double raw_zu =
                static_cast<double>(dig_[static_cast<size_t>(ZU_H)]) * 128.0 +
                static_cast<double>(dig_[static_cast<size_t>(ZU_L)]) * 4.0 +
                static_cast<double>(dig_[static_cast<size_t>(AZ4_H)] >> 3);
            frame_.zu_deg = raw_zu * kScaleZu / 100.0;

            int32_t hx = hx_raw_, hy = hy_raw_, hz = hz_raw_;
            if ((dig_[static_cast<size_t>(HX_H)] >> 4) & 0x01) hx *= 2;
            if ((dig_[static_cast<size_t>(HX_H)] >> 3) & 0x01) hy *= 2;
            if ((dig_[static_cast<size_t>(HX_H)] >> 2) & 0x01) hz *= 2;
            frame_.qual_stat = (dig_[static_cast<size_t>(HX_H)] >> 1) & 0x01;
            compute_surface_inclinometry(hx, hy, hz);
        }
        return;
    }

    /* ========================================================================
     * Format №12 (FORMAT_STD) & Format №13 (FORMAT_STD_RETR)
     * Exact indices from format12_2k.h & format13_2k.h
     * ======================================================================== */
    if (cfg_.format_id == DSP_FMT_STD || cfg_.format_id == DSP_FMT_STD_RETR) {
        const bool r12 = (cfg_.format_id == DSP_FMT_STD);
        const int32_t OTK1_H = r12 ? 1 : 2,   OTK1_L = r12 ? 2 : 3;
        const int32_t ZU_H   = r12 ? 3 : 4,   ZU_L   = r12 ? 4 : 5;
        const int32_t GMOD_H = r12 ? 5 : 6;
        const int32_t AZ1_H  = r12 ? 6 : 7,   AZ1_L  = r12 ? 7 : 8;
        const int32_t OTKu_H = r12 ? 8 : 9,   OTKu_L = r12 ? 9 : 10;
        const int32_t OTK2_H = r12 ? 10 : 12, OTK2_L = r12 ? 11 : 13;
        const int32_t GMOD_L = r12 ? 12 : 14;
        const int32_t HX_H   = r12 ? 13 : 15, HX_M   = r12 ? 14 : 16;
        const int32_t AZ2_H  = r12 ? 15 : 17, AZ2_L  = r12 ? 16 : 18;
        const int32_t HX_L   = r12 ? 17 : 19, HY_H   = r12 ? 18 : 20;
        const int32_t OTK3_H = r12 ? 19 : 22, OTK3_L = r12 ? 20 : 23;
        const int32_t HY_M   = r12 ? 21 : 24, HY_L   = r12 ? 22 : 25;
        const int32_t AZ3_H  = r12 ? 23 : 26, AZ3_L  = r12 ? 24 : 27;
        const int32_t HZ_H   = r12 ? 25 : 28, HZ_L   = r12 ? 26 : 29;
        const int32_t OTK4_H = r12 ? 27 : 31, OTK4_L = r12 ? 28 : 32;
        const int32_t VIBR_H = r12 ? 29 : 33, VIBR_L = r12 ? 30 : 34;
        const int32_t AZ4_H  = r12 ? 31 : 35, AZ4_L  = r12 ? 32 : 36;

        if (kod == OTK1_L)      emit_toolface(DSP_TF_GRAV, kod, decode_otk8(OTK1_H, OTK1_L), reliability_pct, decode_latency_ms, toolface_sink);
        else if (kod == OTK2_L) emit_toolface(DSP_TF_GRAV, kod, decode_otk8(OTK2_H, OTK2_L), reliability_pct, decode_latency_ms, toolface_sink);
        else if (kod == OTK3_L) emit_toolface(DSP_TF_GRAV, kod, decode_otk8(OTK3_H, OTK3_L), reliability_pct, decode_latency_ms, toolface_sink);
        else if (kod == OTK4_L) emit_toolface(DSP_TF_GRAV, kod, decode_otk8(OTK4_H, OTK4_L), reliability_pct, decode_latency_ms, toolface_sink);
        else if (kod == AZ1_L)  emit_toolface(DSP_TF_MAG,  kod, decode_otk8(AZ1_H, AZ1_L),   reliability_pct, decode_latency_ms, toolface_sink);
        else if (kod == AZ2_L)  emit_toolface(DSP_TF_MAG,  kod, decode_otk8(AZ2_H, AZ2_L),   reliability_pct, decode_latency_ms, toolface_sink);
        else if (kod == AZ3_L)  emit_toolface(DSP_TF_MAG,  kod, decode_otk8(AZ3_H, AZ3_L),   reliability_pct, decode_latency_ms, toolface_sink);
        else if (kod == AZ4_L)  emit_toolface(DSP_TF_MAG,  kod, decode_otk8(AZ4_H, AZ4_L),   reliability_pct, decode_latency_ms, toolface_sink);
        else if (kod == OTKu_L) {
            const double ang = static_cast<double>(dig_[static_cast<size_t>(OTKu_H)] * 32 + dig_[static_cast<size_t>(OTKu_L)]) * 0.4;
            emit_toolface(DSP_TF_STAT, kod, ang, reliability_pct, decode_latency_ms, toolface_sink);
        }
        else if (kod == GMOD_L) {
            int16_t t = static_cast<int16_t>(dig_[static_cast<size_t>(GMOD_H)] * 32 + dig_[static_cast<size_t>(GMOD_L)]);
            t = static_cast<int16_t>(t << 6) >> 6;
            frame_.g_total = 1.0 + static_cast<double>(t) / 4096.0;
        }
        else if (kod == HY_H) {
            uint16_t t = 0;
            pars_bits(dig_[static_cast<size_t>(HX_H)], 0, 0, &t, 11);
            pars_bits(dig_[static_cast<size_t>(HX_M)], 4, 0, &t, 10);
            pars_bits(dig_[static_cast<size_t>(HX_L)], 4, 0, &t, 5);
            pars_bits(dig_[static_cast<size_t>(HY_H)], 4, 4, &t, 0);
            hx_raw_ = sign_extend_12(t);
        }
        else if (kod == HY_L) {
            uint16_t t = 0;
            pars_bits(dig_[static_cast<size_t>(HY_H)], 3, 0, &t, 11);
            pars_bits(dig_[static_cast<size_t>(HY_M)], 4, 0, &t, 7);
            pars_bits(dig_[static_cast<size_t>(HY_L)], 4, 2, &t, 2);
            hy_raw_ = sign_extend_12(t);
        }
        else if (kod == HZ_L) {
            uint16_t t = 0;
            pars_bits(dig_[static_cast<size_t>(HY_L)], 1, 0, &t, 11);
            pars_bits(dig_[static_cast<size_t>(HZ_H)], 4, 0, &t, 9);
            pars_bits(dig_[static_cast<size_t>(HZ_L)], 4, 0, &t, 4);
            hz_raw_ = sign_extend_12(t);
        }
        else if (kod == VIBR_L) {
            frame_.vibr_zts = (dig_[static_cast<size_t>(VIBR_H)] & 0x01) * 32 + dig_[static_cast<size_t>(VIBR_L)];
        }
        else if (kod == VIBR_H) {
            const double raw_zu =
                static_cast<double>(dig_[static_cast<size_t>(ZU_H)]) * 128.0 +
                static_cast<double>(dig_[static_cast<size_t>(ZU_L)]) * 4.0 +
                static_cast<double>(dig_[static_cast<size_t>(VIBR_H)] >> 3);
            frame_.zu_deg = raw_zu * kScaleZu / 100.0;

            int32_t hx = hx_raw_, hy = hy_raw_, hz = hz_raw_;
            if ((dig_[static_cast<size_t>(HX_H)] >> 4) & 0x01) hx *= 2;
            if ((dig_[static_cast<size_t>(HX_H)] >> 3) & 0x01) hy *= 2;
            if ((dig_[static_cast<size_t>(HX_H)] >> 2) & 0x01) hz *= 2;
            frame_.qual_stat = (dig_[static_cast<size_t>(HX_H)] >> 1) & 0x01;
            compute_surface_inclinometry(hx, hy, hz);
        }
        return;
    }

    /* ========================================================================
     * Format №2 (FORMAT_NDM_GK_RETR — 104 vectors + 7 pauses, format2_2k.h)
     * ======================================================================== */
    if (cfg_.format_id == DSP_FMT_NDM_GK_RETR) {
        static constexpr int32_t kOtkPairs[7][2]   = {{2,3},{18,19},{34,35},{49,50},{64,65},{79,80},{94,95}};
        static constexpr int32_t kAzPairs[7][2]    = {{8,9},{24,25},{40,41},{55,56},{70,71},{85,86},{100,101}};
        static constexpr int32_t kGkPairs[7][2]    = {{4,5},{20,21},{36,37},{51,52},{66,67},{81,82},{96,97}};
        static constexpr int32_t kGkUpPairs[7][2]  = {{6,7},{22,23},{38,39},{53,54},{68,69},{83,84},{98,99}};
        static constexpr int32_t kGkDnPairs[7][2]  = {{10,11},{26,27},{42,43},{57,58},{72,73},{87,88},{102,103}};

        for (int i = 0; i < 7; ++i) {
            if (kod == kOtkPairs[i][1]) {
                emit_toolface(DSP_TF_GRAV, kod, decode_otk8(kOtkPairs[i][0], kOtkPairs[i][1]), reliability_pct, decode_latency_ms, toolface_sink);
                return;
            }
            if (kod == kAzPairs[i][1]) {
                emit_toolface(DSP_TF_MAG, kod, decode_otk8(kAzPairs[i][0], kAzPairs[i][1]), reliability_pct, decode_latency_ms, toolface_sink);
                return;
            }
            if (kod == kGkPairs[i][1]) {
                frame_.gk_zts = decode_gk8(kGkPairs[i][0], kGkPairs[i][1]);
                return;
            }
            if (kod == kGkUpPairs[i][1]) {
                frame_.gk_up_ndm = decode_gk8(kGkUpPairs[i][0], kGkUpPairs[i][1]);
                return;
            }
            if (kod == kGkDnPairs[i][1]) {
                frame_.gk_dn_ndm = decode_gk8(kGkDnPairs[i][0], kGkDnPairs[i][1]);
                return;
            }
        }

        if (kod == 16) { /* OTKu_L_R2 */
            const double ang = static_cast<double>(dig_[15] * 32 + dig_[16]) * 0.4;
            emit_toolface(DSP_TF_STAT, kod, ang, reliability_pct, decode_latency_ms, toolface_sink);
        } else if (kod == 28) { /* GMOD_L_R2 */
            int16_t t = static_cast<int16_t>(dig_[14] * 32 + dig_[28]);
            t = static_cast<int16_t>(t << 6) >> 6;
            frame_.g_total = 1.0 + static_cast<double>(t) / 4096.0;
        } else if (kod == 47) { /* ZUndm_L_R2 */
            uint16_t t = 0;
            pars_bits(dig_[46], 1, 0, &t, 11);
            pars_bits(dig_[47], 4, 0, &t, 9);
            frame_.zu_ndm_deg = static_cast<double>(t) * kScaleZu / 100.0;
        } else if (kod == 62) { /* TOK_L_R2 */
            frame_.tok_ndm = dig_[61] * 32 + dig_[62];
        } else if (kod == 91) { /* TEMPndm_L_R2 */
            frame_.temp_ndm = static_cast<double>(dig_[90] * 32 + dig_[91]) * 0.25;
        } else if (kod == 100) { /* AZ_OTK7_H_R2 -> finalize ZU & H */
            const double raw_zu =
                static_cast<double>(dig_[12]) * 128.0 +
                static_cast<double>(dig_[13]) * 4.0 +
                static_cast<double>(dig_[100] >> 3);
            frame_.zu_deg = raw_zu * kScaleZu / 100.0;
            frame_.qual_stat = (dig_[45] >> 1) & 0x01;
            compute_surface_inclinometry(hx_raw_, hy_raw_, hz_raw_);
        }
        return;
    }

    /* ========================================================================
     * Format №30 (FORMAT_GEOMASH) & Format №31 (FORMAT_GEOMASH_RETR)
     * ======================================================================== */
    if (cfg_.format_id == DSP_FMT_GEOMASH || cfg_.format_id == DSP_FMT_GEOMASH_RETR) {
        if (kod == 3) { /* OBOR_L_R30/31 */
            uint16_t t = 0;
            pars_bits(dig_[2], 4, 0, &t, 9);
            pars_bits(dig_[3], 4, 0, &t, 4);
            servis_raw_ = t;
            apply_service_param();
        } else if (kod == 4) { /* VIBR_H_R30/31 */
            frame_.qual_stat = (dig_[4] >> 3) & 0x01;
        } else if (kod == 5) { /* VIBR_L_R30/31 */
            frame_.vibr_zts = (dig_[4] & 0x01) * 32 + dig_[5];
        } else if (kod == 7) { /* COUNT_L_R30/31 */
            uint16_t t = 0;
            pars_bits(dig_[6], 4, 0, &t, 9);
            pars_bits(dig_[7], 4, 0, &t, 4);
            geomash_count_ = t;
        }
    }
}

void FrameDecoder::finish_frame() {
    if (cfg_.format_id == DSP_FMT_GEOMASH || cfg_.format_id == DSP_FMT_GEOMASH_RETR) {
        /* Unpack 26 5-bit payload vectors (indices 8..33) -> 16 bytes (join_NBbit_arr) + verify CRC32 */
        uint8_t packed[32]{};
        const uint32_t byte_cnt = join_5bit_to_bytes(&dig_[8], 26, packed);
        if (byte_cnt >= 8) {
            const uint32_t expected_crc =
                static_cast<uint32_t>(packed[byte_cnt - 4]) |
                (static_cast<uint32_t>(packed[byte_cnt - 3]) << 8) |
                (static_cast<uint32_t>(packed[byte_cnt - 2]) << 16) |
                (static_cast<uint32_t>(packed[byte_cnt - 1]) << 24);
            const uint32_t calc_crc = compute_crc32_ieee(packed, byte_cnt - 4);
            frame_.crc_valid = (calc_crc == expected_crc) ? 1 : 0;
        } else {
            frame_.crc_valid = 0;
        }
    }
}

} // namespace dsp

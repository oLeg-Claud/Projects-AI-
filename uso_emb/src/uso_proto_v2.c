#ifndef USO_BOOTLOADER

#include "uso_proto_v2.h"
#include <string.h>

static void put_u16_le(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32_le(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
    dst[2] = (uint8_t)((v >> 16) & 0xFFu);
    dst[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t get_u16_le(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static uint32_t get_u32_le(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

uint16_t uso_v2_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8u; ++bit) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

void uso_v2_default_config(uso_v2_config_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->carrier_freq_mhz   = 5000u; /* 5.0 Hz default MWD carrier */
    cfg->dr_code            = USO_V2_DR_AUTO;
    cfg->decimation         = 0u;    /* auto */
    cfg->pga_gain           = 2u;    /* x4 safe default */
    cfg->adc_filter         = USO_V2_FLT_SINC4;
    cfg->adc_flags          = USO_V2_ADC_FLAG_VBIAS;
    cfg->inpmux             = USO_V2_MUX_AIN8_AIN9;
    cfg->dsp_lpf_mode       = USO_V2_LPF_BYPASS; /* Linear-phase stream for PC FIR */
    cfg->batch_size         = 0u;    /* auto optimal latency */
    cfg->dsp_lpf_cutoff_dhz = 0u;    /* auto */
    cfg->dsp_hpf_cutoff_mhz = 100u;  /* 0.1 Hz DC blocker */
    cfg->stream_flags       = USO_V2_STREAM_AUTO_RECOVER;
}

uint16_t uso_v2_dr_nominal_dhz(uint8_t dr_code, uint32_t adc_clock_hz, bool chop_enabled)
{
    /* Nominal ADS1262 data rates in 0.1 Hz units at fCLK = 7.3728 MHz (TI SBAS661C Table 9-12) */
    uint32_t base_dhz = 0u;
    switch (dr_code) {
        case USO_V2_DR_20:   base_dhz = 200u;   break;
        case USO_V2_DR_50:   base_dhz = 500u;   break;
        case USO_V2_DR_60:   base_dhz = 600u;   break;
        case USO_V2_DR_100:  base_dhz = 1000u;  break;
        case USO_V2_DR_400:  base_dhz = 4000u;  break;
        case USO_V2_DR_1200: base_dhz = 12000u; break;
        case USO_V2_DR_2400: base_dhz = 24000u; break;
        case USO_V2_DR_4800: base_dhz = 48000u; break;
        default:             return 0u;
    }

    if (adc_clock_hz == 0u) adc_clock_hz = 7372800u;

    /* Scale proportionally to actual ADC clock (e.g. 4.608 MHz -> 0.625x -> 400 SPS becomes 250.0 SPS) */
    uint64_t scaled = ((uint64_t)base_dhz * (uint64_t)adc_clock_hz + 3686400ull) / 7372800ull;

    /* Note: TI ADS1262 CHOP mode halves effective settling rate in Sinc4 continuous mode
     * (~1/2 output rate at higher DRs; at 400 SPS Sinc4 CHOP=1 actual DRDY is ~182..200 SPS;
     * for exact timing we keep CHOP off by default unless requested and report actual scaled rate). */
    (void)chop_enabled;
    if (scaled > 65535ull) scaled = 65535ull;
    return (uint16_t)scaled;
}

void uso_v2_select_adaptive_rate(
    uint16_t carrier_freq_mhz,
    uint32_t adc_clock_hz,
    bool     chop_enabled,
    uint8_t  *out_dr_code,
    uint8_t  *out_decim,
    uint8_t  *out_batch,
    uint16_t *out_eff_fs_dhz
)
{
    if (carrier_freq_mhz < 500u) carrier_freq_mhz = 5000u;
    if (adc_clock_hz == 0u) adc_clock_hz = 7372800u;

    /* Virtual grid rate in 0.1 Hz units = 26 * f_carrier (DLPOL_R = 13 per half-period).
     * Target Fs >= 1.15 * virtual_rate (or >= 0.95x for 16 Hz where 400 SPS gives 12.5 sps/bit). */
    uint32_t virtual_dhz = ((uint32_t)carrier_freq_mhz * 26u) / 100u;
    uint32_t min_target_dhz = (carrier_freq_mhz >= 14000u)
                                  ? ((virtual_dhz * 90u) / 100u)
                                  : ((virtual_dhz * 115u) / 100u);

    /* Candidate hardware DRs from lowest noise (lowest SPS) to fastest */
    static const uint8_t kCandidates[] = {
        USO_V2_DR_50,
        USO_V2_DR_60,
        USO_V2_DR_100,
        USO_V2_DR_400,
        USO_V2_DR_1200
    };

    uint8_t chosen_dr = USO_V2_DR_400;
    uint8_t chosen_decim = 1u;
    uint16_t chosen_fs_dhz = uso_v2_dr_nominal_dhz(USO_V2_DR_400, adc_clock_hz, chop_enabled);

    for (size_t i = 0; i < sizeof(kCandidates); ++i) {
        uint8_t dr = kCandidates[i];
        uint16_t hw_dhz = uso_v2_dr_nominal_dhz(dr, adc_clock_hz, chop_enabled);
        if (hw_dhz >= min_target_dhz) {
            chosen_dr = dr;
            chosen_decim = 1u;
            chosen_fs_dhz = hw_dhz;
            /* If 400 SPS is selected for 5..7 Hz carrier, integer decimation by 2 -> 200.0 SPS
             * cuts white noise by 3 dB and halves USB traffic while staying well above 130..182 Hz! */
            if (dr == USO_V2_DR_400 && hw_dhz >= 3600u && (hw_dhz / 2u) >= min_target_dhz) {
                chosen_decim = 2u;
                chosen_fs_dhz = (uint16_t)(hw_dhz / 2u);
            }
            break;
        }
    }

    /* Choose batch size targeting ~20..30 ms USB packet cadence (1..10 samples per 61-byte frame) */
    uint32_t batch = ((uint32_t)chosen_fs_dhz * 25u + 5000u) / 10000u; /* ~25 ms */
    if (batch < 1u) batch = 1u;
    if (batch > USO_V2_MAX_BATCH_SAMPLES) batch = USO_V2_MAX_BATCH_SAMPLES;

    if (out_dr_code)    *out_dr_code    = chosen_dr;
    if (out_decim)      *out_decim      = chosen_decim;
    if (out_batch)      *out_batch      = (uint8_t)batch;
    if (out_eff_fs_dhz) *out_eff_fs_dhz = chosen_fs_dhz;
}

unsigned uso_v2_encode_frame(
    uint8_t       magic0,
    uint8_t       magic1,
    uint8_t       flags,
    uint8_t       msg_type,
    uint16_t      seq,
    const uint8_t *payload,
    uint8_t       payload_len,
    uint8_t       out[USO_V2_MAX_FRAME]
)
{
    if (!out || payload_len > USO_V2_MAX_PAYLOAD) return 0u;
    out[0] = magic0;
    out[1] = magic1;
    out[2] = (uint8_t)((USO_V2_PROTO_VERSION << 4) | (flags & 0x0Fu));
    out[3] = msg_type;
    put_u16_le(out + 4, seq);
    out[6] = payload_len;
    if (payload_len > 0u && payload) {
        memcpy(out + USO_V2_HDR_SIZE, payload, payload_len);
    }
    uint16_t crc = uso_v2_crc16(out, (size_t)(USO_V2_HDR_SIZE + payload_len));
    put_u16_le(out + USO_V2_HDR_SIZE + payload_len, crc);
    return (unsigned)(USO_V2_OVERHEAD + payload_len);
}

void uso_v2_pack_config(const uso_v2_config_t *cfg, uint8_t out[USO_V2_CFG_WIRE_LEN])
{
    put_u16_le(out + 0, cfg->carrier_freq_mhz);
    out[2]  = cfg->dr_code;
    out[3]  = cfg->decimation;
    out[4]  = cfg->pga_gain;
    out[5]  = cfg->adc_filter;
    out[6]  = cfg->adc_flags;
    out[7]  = cfg->inpmux;
    out[8]  = cfg->dsp_lpf_mode;
    out[9]  = cfg->batch_size;
    put_u16_le(out + 10, cfg->dsp_lpf_cutoff_dhz);
    put_u16_le(out + 12, cfg->dsp_hpf_cutoff_mhz);
    out[14] = cfg->stream_flags;
    out[15] = cfg->reserved;
}

bool uso_v2_unpack_config(const uint8_t *in, uint8_t len, uso_v2_config_t *cfg)
{
    if (!in || !cfg || len != USO_V2_CFG_WIRE_LEN) return false;
    cfg->carrier_freq_mhz   = get_u16_le(in + 0);
    cfg->dr_code            = in[2];
    cfg->decimation         = in[3];
    cfg->pga_gain           = in[4];
    cfg->adc_filter         = in[5];
    cfg->adc_flags          = in[6];
    cfg->inpmux             = in[7];
    cfg->dsp_lpf_mode       = in[8];
    cfg->batch_size         = in[9];
    cfg->dsp_lpf_cutoff_dhz = get_u16_le(in + 10);
    cfg->dsp_hpf_cutoff_mhz = get_u16_le(in + 12);
    cfg->stream_flags       = in[14];
    cfg->reserved           = in[15];
    return true;
}

void uso_v2_pack_live(const uso_v2_live_t *live, uint8_t out[USO_V2_LIVE_WIRE_LEN])
{
    out[0] = live->mask;
    out[1] = live->pga_gain;
    out[2] = live->dsp_lpf_mode;
    out[3] = live->batch_size;
    put_u16_le(out + 4, live->dsp_lpf_cutoff_dhz);
    put_u16_le(out + 6, live->dsp_hpf_cutoff_mhz);
}

bool uso_v2_unpack_live(const uint8_t *in, uint8_t len, uso_v2_live_t *live)
{
    if (!in || !live || len != USO_V2_LIVE_WIRE_LEN) return false;
    live->mask               = in[0];
    live->pga_gain           = in[1];
    live->dsp_lpf_mode       = in[2];
    live->batch_size         = in[3];
    live->dsp_lpf_cutoff_dhz = get_u16_le(in + 4);
    live->dsp_hpf_cutoff_mhz = get_u16_le(in + 6);
    return true;
}

void uso_v2_pack_hello_rsp(const uso_v2_hello_rsp_t *rsp, uint8_t out[USO_V2_HELLO_RSP_WIRE_LEN])
{
    out[0] = rsp->proto_version;
    out[1] = rsp->device_id;
    out[2] = rsp->fw_major;
    out[3] = rsp->fw_minor;
    out[4] = rsp->fw_patch;
    out[5] = rsp->adc_id;
    out[6] = rsp->ready;
    out[7] = rsp->fault;
    put_u32_le(out + 8, rsp->capabilities);
    put_u32_le(out + 12, rsp->adc_clock_hz);
    memcpy(out + 16, rsp->mcu_uid, 12);
}

bool uso_v2_unpack_hello_rsp(const uint8_t *in, uint8_t len, uso_v2_hello_rsp_t *rsp)
{
    if (!in || !rsp || len != USO_V2_HELLO_RSP_WIRE_LEN) return false;
    rsp->proto_version = in[0];
    rsp->device_id     = in[1];
    rsp->fw_major      = in[2];
    rsp->fw_minor      = in[3];
    rsp->fw_patch      = in[4];
    rsp->adc_id        = in[5];
    rsp->ready         = in[6];
    rsp->fault         = in[7];
    rsp->capabilities  = get_u32_le(in + 8);
    rsp->adc_clock_hz  = get_u32_le(in + 12);
    memcpy(rsp->mcu_uid, in + 16, 12);
    return true;
}

void uso_v2_pack_start_rsp(const uso_v2_start_rsp_t *rsp, uint8_t out[USO_V2_START_RSP_WIRE_LEN])
{
    out[0] = rsp->status;
    out[1] = rsp->dr_code;
    out[2] = rsp->decimation;
    out[3] = rsp->batch_size;
    out[4] = rsp->pga_gain;
    out[5] = rsp->adc_filter;
    out[6] = rsp->adc_flags;
    out[7] = rsp->inpmux;
    put_u16_le(out + 8, rsp->effective_fs_dhz);
    put_u16_le(out + 10, rsp->hw_fs_dhz);
}

bool uso_v2_unpack_start_rsp(const uint8_t *in, uint8_t len, uso_v2_start_rsp_t *rsp)
{
    if (!in || !rsp || len != USO_V2_START_RSP_WIRE_LEN) return false;
    rsp->status           = in[0];
    rsp->dr_code          = in[1];
    rsp->decimation       = in[2];
    rsp->batch_size       = in[3];
    rsp->pga_gain         = in[4];
    rsp->adc_filter       = in[5];
    rsp->adc_flags        = in[6];
    rsp->inpmux           = in[7];
    rsp->effective_fs_dhz = get_u16_le(in + 8);
    rsp->hw_fs_dhz        = get_u16_le(in + 10);
    return true;
}

void uso_v2_pack_diag_rsp(const uso_v2_diag_rsp_t *rsp, uint8_t out[USO_V2_DIAG_RSP_WIRE_LEN])
{
    put_u32_le(out + 0,  rsp->uptime_ms);
    put_u32_le(out + 4,  rsp->samples);
    put_u32_le(out + 8,  rsp->dropped_samples);
    put_u32_le(out + 12, rsp->missed_drdy);
    put_u32_le(out + 16, rsp->checksum_errors);
    put_u32_le(out + 20, rsp->adc_errors);
    put_u32_le(out + 24, rsp->stale_count);
    put_u32_le(out + 28, rsp->tx_overflows);
    put_u32_le(out + 32, rsp->rx_overflows);
    put_u32_le(out + 36, rsp->v2_crc_errors);
    put_u16_le(out + 40, rsp->effective_fs_dhz);
    out[42] = rsp->adc_status;
    out[43] = rsp->state_flags;
}

bool uso_v2_unpack_diag_rsp(const uint8_t *in, uint8_t len, uso_v2_diag_rsp_t *rsp)
{
    if (!in || !rsp || len != USO_V2_DIAG_RSP_WIRE_LEN) return false;
    rsp->uptime_ms        = get_u32_le(in + 0);
    rsp->samples          = get_u32_le(in + 4);
    rsp->dropped_samples  = get_u32_le(in + 8);
    rsp->missed_drdy      = get_u32_le(in + 12);
    rsp->checksum_errors  = get_u32_le(in + 16);
    rsp->adc_errors       = get_u32_le(in + 20);
    rsp->stale_count      = get_u32_le(in + 24);
    rsp->tx_overflows     = get_u32_le(in + 28);
    rsp->rx_overflows     = get_u32_le(in + 32);
    rsp->v2_crc_errors    = get_u32_le(in + 36);
    rsp->effective_fs_dhz = get_u16_le(in + 40);
    rsp->adc_status       = in[42];
    rsp->state_flags      = in[43];
    return true;
}

unsigned uso_v2_encode_batch(
    uint16_t             seq,
    const uso_v2_batch_t *batch,
    uint8_t              out[USO_V2_MAX_FRAME]
)
{
    if (!batch || !out || batch->sample_count == 0u || batch->sample_count > USO_V2_MAX_BATCH_SAMPLES) {
        return 0u;
    }
    uint8_t payload[USO_V2_MAX_PAYLOAD];
    put_u32_le(payload + 0, batch->timestamp_ms);
    put_u32_le(payload + 4, batch->first_sample_idx);
    put_u16_le(payload + 8, batch->effective_fs_dhz);
    payload[10] = batch->flags;
    payload[11] = (uint8_t)(((batch->pga_gain & 0x0Fu) << 4) | (batch->sample_count & 0x0Fu));
    for (uint8_t i = 0; i < batch->sample_count; ++i) {
        uint32_t raw;
        memcpy(&raw, &batch->samples[i], sizeof(raw));
        put_u32_le(payload + USO_V2_BATCH_HDR_SIZE + (size_t)i * 4u, raw);
    }
    uint8_t plen = (uint8_t)(USO_V2_BATCH_HDR_SIZE + batch->sample_count * 4u);
    return uso_v2_encode_frame(
        USO_V2_MAGIC_TX0,
        USO_V2_MAGIC_TX1,
        0u,
        USO_V2_MSG_DATA_BATCH,
        seq,
        payload,
        plen,
        out
    );
}

bool uso_v2_unpack_batch(const uint8_t *payload, uint8_t payload_len, uso_v2_batch_t *batch)
{
    if (!payload || !batch || payload_len < USO_V2_BATCH_HDR_SIZE + 4u) return false;
    uint8_t count = payload[11] & 0x0Fu;
    if (count == 0u || count > USO_V2_MAX_BATCH_SAMPLES) return false;
    if (payload_len != (uint8_t)(USO_V2_BATCH_HDR_SIZE + count * 4u)) return false;

    batch->timestamp_ms     = get_u32_le(payload + 0);
    batch->first_sample_idx = get_u32_le(payload + 4);
    batch->effective_fs_dhz = get_u16_le(payload + 8);
    batch->flags            = payload[10];
    batch->pga_gain         = (uint8_t)((payload[11] >> 4) & 0x0Fu);
    batch->sample_count     = count;
    for (uint8_t i = 0; i < count; ++i) {
        uint32_t raw = get_u32_le(payload + USO_V2_BATCH_HDR_SIZE + (size_t)i * 4u);
        memcpy(&batch->samples[i], &raw, sizeof(raw));
    }
    return true;
}

enum {
    V2_ST_IDLE = 0,
    V2_ST_MAGIC1 = 1,
    V2_ST_HEADER = 2,
    V2_ST_BODY = 3
};

void uso_v2_parser_init(uso_v2_parser_t *p, uint8_t magic0, uint8_t magic1)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->expect_magic0 = magic0;
    p->expect_magic1 = magic1;
}

void uso_v2_parser_reset(uso_v2_parser_t *p)
{
    if (!p) return;
    p->state = V2_ST_IDLE;
    p->pos = 0u;
    p->expected_total = 0u;
}

void uso_v2_parser_expire(uso_v2_parser_t *p, uint32_t now_ms, uint32_t timeout_ms)
{
    if (!p || p->state == V2_ST_IDLE) return;
    if ((uint32_t)(now_ms - p->since_ms) >= timeout_ms) {
        uso_v2_parser_reset(p);
    }
}

bool uso_v2_parser_active(const uso_v2_parser_t *p)
{
    return p && p->state != V2_ST_IDLE;
}

uso_v2_parse_res_t uso_v2_parser_feed(
    uso_v2_parser_t *p,
    uint8_t          byte,
    uint32_t         now_ms,
    uint32_t         timeout_ms,
    uso_v2_frame_t  *out_frame
)
{
    if (!p) return USO_V2_PARSE_NONE;
    uso_v2_parser_expire(p, now_ms, timeout_ms);

    switch (p->state) {
        case V2_ST_IDLE:
            if (byte == p->expect_magic0) {
                p->buf[0] = byte;
                p->pos = 1u;
                p->since_ms = now_ms;
                p->state = V2_ST_MAGIC1;
            }
            return USO_V2_PARSE_NONE;

        case V2_ST_MAGIC1:
            if (byte == p->expect_magic1) {
                p->buf[1] = byte;
                p->pos = 2u;
                p->state = V2_ST_HEADER;
                return USO_V2_PARSE_NONE;
            }
            /* Not a V2 frame! If byte is magic0 again, stay in MAGIC1; else let legacy handle byte. */
            if (byte == p->expect_magic0) {
                p->since_ms = now_ms;
                return USO_V2_PARSE_NONE;
            }
            uso_v2_parser_reset(p);
            return USO_V2_PARSE_NOT_V2;

        case V2_ST_HEADER:
            p->buf[p->pos++] = byte;
            if (p->pos == USO_V2_HDR_SIZE) {
                uint8_t ver = (uint8_t)(p->buf[2] >> 4);
                uint8_t plen = p->buf[6];
                if (ver != USO_V2_PROTO_VERSION || plen > USO_V2_MAX_PAYLOAD) {
                    ++p->crc_errors;
                    uso_v2_parser_reset(p);
                    return USO_V2_PARSE_BAD_CRC;
                }
                p->expected_total = (uint8_t)(USO_V2_OVERHEAD + plen);
                p->state = V2_ST_BODY;
            }
            return USO_V2_PARSE_NONE;

        case V2_ST_BODY:
            p->buf[p->pos++] = byte;
            if (p->pos >= p->expected_total) {
                uint8_t plen = p->buf[6];
                uint16_t rx_crc = get_u16_le(p->buf + USO_V2_HDR_SIZE + plen);
                uint16_t calc_crc = uso_v2_crc16(p->buf, (size_t)(USO_V2_HDR_SIZE + plen));
                if (rx_crc != calc_crc) {
                    ++p->crc_errors;
                    uso_v2_parser_reset(p);
                    return USO_V2_PARSE_BAD_CRC;
                }
                if (out_frame) {
                    out_frame->version     = (uint8_t)(p->buf[2] >> 4);
                    out_frame->flags       = (uint8_t)(p->buf[2] & 0x0Fu);
                    out_frame->msg_type    = p->buf[3];
                    out_frame->seq         = get_u16_le(p->buf + 4);
                    out_frame->payload_len = plen;
                    if (plen > 0u) {
                        memcpy(out_frame->payload, p->buf + USO_V2_HDR_SIZE, plen);
                    }
                }
                uso_v2_parser_reset(p);
                return USO_V2_PARSE_FRAME;
            }
            return USO_V2_PARSE_NONE;

        default:
            uso_v2_parser_reset(p);
            return USO_V2_PARSE_NONE;
    }
}

#endif /* !USO_BOOTLOADER */

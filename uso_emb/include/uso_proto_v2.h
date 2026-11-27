#ifndef USO_PROTO_V2_H
#define USO_PROTO_V2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * USO-32 Dual-Mode Protocol V2 (USO-NG)
 *
 * Designed for 100% automatic coexistence with legacy PROWODKA protocol:
 *   - Legacy commands: 0xAA, 0x66, 0x71, 0x72, 0x20..25+freq, 0x90..95+freq,
 *     0x10, 0xA1 (INFO), 0x08 12 34 56 78 (BOOT).
 *   - V2 Host->USO sync magic: 0xA5, 0x5A (neither byte is a legacy command).
 *   - V2 USO->Host sync magic: 0x5A, 0xA5.
 *   - Max wire frame = 63 bytes (< 64-byte USB FS bulk endpoint -> zero ZLP).
 *   - Little-endian multibyte fields, CRC-16/CCITT-FALSE over bytes [0..7+len-1].
 * ============================================================================ */

#define USO_V2_PROTO_VERSION    2u
#define USO_V2_MAGIC_RX0        0xA5u
#define USO_V2_MAGIC_RX1        0x5Au
#define USO_V2_MAGIC_TX0        0x5Au
#define USO_V2_MAGIC_TX1        0xA5u

#define USO_V2_HDR_SIZE         7u
#define USO_V2_CRC_SIZE         2u
#define USO_V2_OVERHEAD         (USO_V2_HDR_SIZE + USO_V2_CRC_SIZE) /* 9 bytes */
#define USO_V2_MAX_PAYLOAD      54u /* 9 + 54 = 63 bytes < 64-byte USB FS MPS */
#define USO_V2_MAX_FRAME        (USO_V2_OVERHEAD + USO_V2_MAX_PAYLOAD)

#define USO_V2_BATCH_HDR_SIZE   12u
#define USO_V2_MAX_BATCH_SAMPLES 10u /* 12 + 10*4 = 52 payload -> 61 bytes total */

/* Message types */
typedef enum {
    /* Host -> USO commands */
    USO_V2_CMD_HELLO       = 0x01u,
    USO_V2_CMD_CONFIGURE   = 0x02u,
    USO_V2_CMD_START       = 0x03u,
    USO_V2_CMD_STOP        = 0x04u,
    USO_V2_CMD_SET_LIVE    = 0x05u,
    USO_V2_CMD_GET_DIAG    = 0x06u,
    USO_V2_CMD_RECALIBRATE = 0x07u,
    USO_V2_CMD_RESET_ADC   = 0x08u,

    /* USO -> Host responses & telemetry */
    USO_V2_RSP_ACK         = 0x80u,
    USO_V2_RSP_HELLO       = 0x81u,
    USO_V2_RSP_START       = 0x83u,
    USO_V2_RSP_DIAG        = 0x86u,
    USO_V2_RSP_CAL         = 0x87u,
    USO_V2_MSG_DATA_BATCH  = 0x90u
} uso_v2_msg_type_t;

/* Status codes */
typedef enum {
    USO_V2_OK              = 0u,
    USO_V2_ERR_BAD_LEN     = 1u,
    USO_V2_ERR_BAD_PARAM   = 2u,
    USO_V2_ERR_ADC_FAULT   = 3u,
    USO_V2_ERR_BUSY        = 4u,
    USO_V2_ERR_CAL_FAIL    = 5u,
    USO_V2_ERR_UNKNOWN_CMD = 6u
} uso_v2_status_t;

/* ADS1262 MODE2[3:0] data-rate codes */
typedef enum {
    USO_V2_DR_20    = 0x04u,
    USO_V2_DR_50    = 0x05u,
    USO_V2_DR_60    = 0x06u,
    USO_V2_DR_100   = 0x07u,
    USO_V2_DR_400   = 0x08u,
    USO_V2_DR_1200  = 0x09u,
    USO_V2_DR_2400  = 0x0Au,
    USO_V2_DR_4800  = 0x0Bu,
    USO_V2_DR_AUTO  = 0xFFu
} uso_v2_dr_code_t;

/* ADS1262 MODE1[7:5] digital filter modes */
typedef enum {
    USO_V2_FLT_SINC1 = 0u,
    USO_V2_FLT_SINC2 = 1u,
    USO_V2_FLT_SINC3 = 2u,
    USO_V2_FLT_SINC4 = 3u,
    USO_V2_FLT_FIR   = 4u  /* Valid only at <= 20 SPS */
} uso_v2_adc_filter_t;

/* ADS1262 INPMUX presets */
typedef enum {
    USO_V2_MUX_AIN8_AIN9 = 0x89u, /* Normal seismic transformer differential input */
    USO_V2_MUX_SHORT     = 0xFFu, /* Internal short (noise floor & offset check) */
    USO_V2_MUX_TEMP      = 0xBBu, /* Internal ADS1262 temperature sensor */
    USO_V2_MUX_AVDD      = 0xCCu, /* Analog supply monitor (AVDD-AVSS)/4 */
    USO_V2_MUX_DVDD      = 0xDDu  /* Digital supply monitor DVDD/4 */
} uso_v2_inpmux_t;

/* On-MCU software LPF modes */
typedef enum {
    USO_V2_LPF_BYPASS   = 0u, /* Linear-phase raw ADC stream (recommended for PC FIR) */
    USO_V2_LPF_BUTTER4  = 1u, /* Adaptive 4th-order Butterworth IIR for active Fs */
    USO_V2_LPF_LEGACY10 = 2u  /* Legacy 10th-order Butterworth (Fs=400 Hz table) */
} uso_v2_lpf_mode_t;

/* Feature capability bitmask (RSP_HELLO.caps) */
#define USO_V2_CAP_ADAPTIVE_RATE  (1u << 0)
#define USO_V2_CAP_HW_FILTERS     (1u << 1)
#define USO_V2_CAP_CHOP           (1u << 2)
#define USO_V2_CAP_RECALIBRATE    (1u << 3)
#define USO_V2_CAP_SOFT_RESET_ADC (1u << 4)
#define USO_V2_CAP_DIAG_MUX       (1u << 5)
#define USO_V2_CAP_LEGACY_FALLBACK (1u << 6)

/* adc_flags bitmask in uso_v2_config_t */
#define USO_V2_ADC_FLAG_CHOP      (1u << 0)
#define USO_V2_ADC_FLAG_VBIAS     (1u << 1)
#define USO_V2_ADC_FLAG_PGA_BYP   (1u << 2)

/* stream_flags bitmask in uso_v2_config_t */
#define USO_V2_STREAM_AUTO_RECOVER (1u << 0)

/* Batch frame flags (MSG_DATA_BATCH.flags) */
#define USO_V2_BATCH_PGA_OVF      (1u << 0) /* ADS1262 PGA overrange or int32 clip */
#define USO_V2_BATCH_ADC_ALARM    (1u << 1) /* Transient REF/RESET/SPI alarm recovered */
#define USO_V2_BATCH_GAP_DROPPED  (1u << 2) /* Missed DRDY or TX drop before this batch */
#define USO_V2_BATCH_HPF_ACTIVE   (1u << 3)
#define USO_V2_BATCH_LPF_ACTIVE   (1u << 4)
#define USO_V2_BATCH_CHOP_ACTIVE  (1u << 5)

/* Decoded V2 configuration (16 bytes on wire in CMD_CONFIGURE) */
typedef struct {
    uint16_t carrier_freq_mhz;   /* Downhole carrier frequency in mHz (e.g. 5000 = 5.0 Hz) */
    uint8_t  dr_code;            /* uso_v2_dr_code_t or USO_V2_DR_AUTO (0xFF) */
    uint8_t  decimation;         /* 0 = auto, or 1..16 */
    uint8_t  pga_gain;           /* 0..5 = x1..x32 */
    uint8_t  adc_filter;         /* uso_v2_adc_filter_t (0..4) */
    uint8_t  adc_flags;          /* USO_V2_ADC_FLAG_* */
    uint8_t  inpmux;             /* uso_v2_inpmux_t (default 0x89) */
    uint8_t  dsp_lpf_mode;       /* uso_v2_lpf_mode_t */
    uint8_t  batch_size;         /* 0 = auto, or 1..10 */
    uint16_t dsp_lpf_cutoff_dhz; /* LPF cutoff in 0.1 Hz (0 = auto ~2.5*fc) */
    uint16_t dsp_hpf_cutoff_mhz; /* HPF cutoff in mHz (0 = OFF, 100 = 0.1 Hz) */
    uint8_t  stream_flags;       /* USO_V2_STREAM_* */
    uint8_t  reserved;
} uso_v2_config_t;

#define USO_V2_CFG_WIRE_LEN 16u

/* Live update payload (8 bytes on wire in CMD_SET_LIVE) */
typedef struct {
    uint8_t  mask;               /* bit0=gain, bit1=hpf, bit2=lpf, bit3=batch */
    uint8_t  pga_gain;           /* 0..5 */
    uint8_t  dsp_lpf_mode;       /* uso_v2_lpf_mode_t */
    uint8_t  batch_size;         /* 0 = keep/auto, 1..10 */
    uint16_t dsp_lpf_cutoff_dhz; /* 0.1 Hz */
    uint16_t dsp_hpf_cutoff_mhz; /* mHz (0 = OFF) */
} uso_v2_live_t;

#define USO_V2_LIVE_WIRE_LEN 8u
#define USO_V2_LIVE_MASK_GAIN  (1u << 0)
#define USO_V2_LIVE_MASK_HPF   (1u << 1)
#define USO_V2_LIVE_MASK_LPF   (1u << 2)
#define USO_V2_LIVE_MASK_BATCH (1u << 3)

/* Resolved active stream parameters (returned in RSP_START, 12 bytes) */
typedef struct {
    uint8_t  status;
    uint8_t  dr_code;
    uint8_t  decimation;
    uint8_t  batch_size;
    uint8_t  pga_gain;
    uint8_t  adc_filter;
    uint8_t  adc_flags;
    uint8_t  inpmux;
    uint16_t effective_fs_dhz;   /* Actual output sample rate in 0.1 Hz */
    uint16_t hw_fs_dhz;          /* Raw ADS1262 sample rate in 0.1 Hz */
} uso_v2_start_rsp_t;

#define USO_V2_START_RSP_WIRE_LEN 12u

/* RSP_HELLO payload (28 bytes) */
typedef struct {
    uint8_t  proto_version;
    uint8_t  device_id;
    uint8_t  fw_major;
    uint8_t  fw_minor;
    uint8_t  fw_patch;
    uint8_t  adc_id;
    uint8_t  ready;
    uint8_t  fault;
    uint32_t capabilities;
    uint32_t adc_clock_hz;
    uint8_t  mcu_uid[12];
} uso_v2_hello_rsp_t;

#define USO_V2_HELLO_RSP_WIRE_LEN 28u

/* RSP_DIAG payload (44 bytes) */
typedef struct {
    uint32_t uptime_ms;
    uint32_t samples;
    uint32_t dropped_samples;
    uint32_t missed_drdy;
    uint32_t checksum_errors;
    uint32_t adc_errors;
    uint32_t stale_count;
    uint32_t tx_overflows;
    uint32_t rx_overflows;
    uint32_t v2_crc_errors;
    uint16_t effective_fs_dhz;
    uint8_t  adc_status;
    uint8_t  state_flags; /* bit0=ready, bit1=running, bit2=fault, bit3=v2_mode */
} uso_v2_diag_rsp_t;

#define USO_V2_DIAG_RSP_WIRE_LEN 44u

/* Decoded MSG_DATA_BATCH */
typedef struct {
    uint32_t timestamp_ms;
    uint32_t first_sample_idx;
    uint16_t effective_fs_dhz;
    uint8_t  flags;
    uint8_t  pga_gain;
    uint8_t  sample_count;
    int32_t  samples[USO_V2_MAX_BATCH_SAMPLES];
} uso_v2_batch_t;

/* Decoded generic V2 frame */
typedef struct {
    uint8_t  version;
    uint8_t  flags;
    uint8_t  msg_type;
    uint16_t seq;
    uint8_t  payload_len;
    uint8_t  payload[USO_V2_MAX_PAYLOAD];
} uso_v2_frame_t;

/* Incremental zero-allocation stream parser */
typedef struct {
    uint8_t  state;
    uint8_t  expect_magic0;
    uint8_t  expect_magic1;
    uint8_t  buf[USO_V2_MAX_FRAME];
    uint8_t  pos;
    uint8_t  expected_total;
    uint32_t since_ms;
    uint32_t crc_errors;
} uso_v2_parser_t;

typedef enum {
    USO_V2_PARSE_NONE     = 0,
    USO_V2_PARSE_FRAME    = 1,
    USO_V2_PARSE_BAD_CRC  = 2,
    USO_V2_PARSE_NOT_V2   = 3  /* First byte was magic0, second byte was NOT magic1 */
} uso_v2_parse_res_t;

/* Pure portable protocol functions */
uint16_t uso_v2_crc16(const uint8_t *data, size_t len);
void     uso_v2_default_config(uso_v2_config_t *cfg);
bool uso_v2_resolve_config(const uso_v2_config_t *cfg, uint32_t clk_hz, uso_v2_start_rsp_t *out);

uint16_t uso_v2_dr_nominal_dhz(uint8_t dr_code, uint32_t adc_clock_hz, bool chop_enabled);
void     uso_v2_select_adaptive_rate(
    uint16_t carrier_freq_mhz,
    uint32_t adc_clock_hz,
    bool     chop_enabled,
    uint8_t  *out_dr_code,
    uint8_t  *out_decim,
    uint8_t  *out_batch,
    uint16_t *out_eff_fs_dhz
);

/* Wire encode/decode helpers */
unsigned uso_v2_encode_frame(
    uint8_t       magic0,
    uint8_t       magic1,
    uint8_t       flags,
    uint8_t       msg_type,
    uint16_t      seq,
    const uint8_t *payload,
    uint8_t       payload_len,
    uint8_t       out[USO_V2_MAX_FRAME]
);

void uso_v2_pack_config(const uso_v2_config_t *cfg, uint8_t out[USO_V2_CFG_WIRE_LEN]);
bool uso_v2_unpack_config(const uint8_t *in, uint8_t len, uso_v2_config_t *cfg);

void uso_v2_pack_live(const uso_v2_live_t *live, uint8_t out[USO_V2_LIVE_WIRE_LEN]);
bool uso_v2_unpack_live(const uint8_t *in, uint8_t len, uso_v2_live_t *live);

void uso_v2_pack_hello_rsp(const uso_v2_hello_rsp_t *rsp, uint8_t out[USO_V2_HELLO_RSP_WIRE_LEN]);
bool uso_v2_unpack_hello_rsp(const uint8_t *in, uint8_t len, uso_v2_hello_rsp_t *rsp);

void uso_v2_pack_start_rsp(const uso_v2_start_rsp_t *rsp, uint8_t out[USO_V2_START_RSP_WIRE_LEN]);
bool uso_v2_unpack_start_rsp(const uint8_t *in, uint8_t len, uso_v2_start_rsp_t *rsp);

void uso_v2_pack_diag_rsp(const uso_v2_diag_rsp_t *rsp, uint8_t out[USO_V2_DIAG_RSP_WIRE_LEN]);
bool uso_v2_unpack_diag_rsp(const uint8_t *in, uint8_t len, uso_v2_diag_rsp_t *rsp);

unsigned uso_v2_encode_batch(
    uint16_t       seq,
    const uso_v2_batch_t *batch,
    uint8_t        out[USO_V2_MAX_FRAME]
);
bool uso_v2_unpack_batch(const uint8_t *payload, uint8_t payload_len, uso_v2_batch_t *batch);

/* Incremental parser */
void uso_v2_parser_init(uso_v2_parser_t *p, uint8_t magic0, uint8_t magic1);
void uso_v2_parser_reset(uso_v2_parser_t *p);
void uso_v2_parser_expire(uso_v2_parser_t *p, uint32_t now_ms, uint32_t timeout_ms);
bool uso_v2_parser_active(const uso_v2_parser_t *p);
uso_v2_parse_res_t uso_v2_parser_feed(
    uso_v2_parser_t *p,
    uint8_t          byte,
    uint32_t         now_ms,
    uint32_t         timeout_ms,
    uso_v2_frame_t  *out_frame
);

#ifdef __cplusplus
}
#endif

#endif /* USO_PROTO_V2_H */

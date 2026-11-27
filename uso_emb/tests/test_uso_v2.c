#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/ads1262.h"
#include "../include/dsp.h"
#include "../include/protocol.h"
#include "../include/uso_config.h"
#include "../include/uso_proto_v2.h"

static void test_legacy_protocol_100pct_preserved(void)
{
    uso_parser_t p;
    memset(&p, 0, sizeof(p));

    assert(uso_parse(&p, 0xAA, 10).kind == USO_READY);
    assert(uso_parse(&p, 0x66, 20).kind == USO_NUMBER);
    assert(uso_parse(&p, 0x71, 30).kind == USO_HP_ON);
    assert(uso_parse(&p, 0x72, 40).kind == USO_HP_OFF);
    assert(uso_parse(&p, 0x10, 50).kind == USO_STOP);
    assert(uso_parse(&p, 0xA1, 60).kind == USO_INFO);

    /* 32-bit START pair: 0x95 (x32) + 0x03 (9 Hz) */
    assert(uso_parse(&p, 0x95, 100).kind == USO_NONE);
    uso_event_t s = uso_parse(&p, 0x03, 120);
    assert(s.kind == USO_START && s.bits32 && s.gain == 5 && s.freq == 0x03);

    /* 24-bit START with freq = 0x10 (11 Hz, NOT STOP!) */
    assert(uso_parse(&p, 0x22, 200).kind == USO_NONE);
    uso_event_t s24 = uso_parse(&p, 0x10, 210);
    assert(s24.kind == USO_START && !s24.bits32 && s24.gain == 2 && s24.freq == 0x10);

    /* Bootloader entry key: 08 12 34 56 78 */
    assert(uso_parse(&p, 0x08, 300).kind == USO_NONE);
    assert(uso_parse(&p, 0x12, 305).kind == USO_NONE);
    assert(uso_parse(&p, 0x34, 310).kind == USO_NONE);
    assert(uso_parse(&p, 0x56, 315).kind == USO_NONE);
    assert(uso_parse(&p, 0x78, 320).kind == USO_BOOT);

    /* USO_NUMBER frame format */
    uint8_t num[5];
    assert(uso_pack_number(num) == 5);
    assert(num[0] == 0x01 && num[1] == USO_DEVICE_NUMBER && num[2] == 0 && num[3] == 0 && num[4] == 0xFF);

    /* Legacy 6-byte sample packing normal & overflow */
    uint8_t pkt[6];
    assert(uso_pack_sample(0x12345678, true, pkt) == 6);
    assert(pkt[0] == 0x00 && pkt[1] == 0x12 && pkt[4] == 0x78 && pkt[5] == 0xFF);
    assert(uso_pack_sample_ex(0x12345678, true, true, pkt) == 6);
    assert(pkt[5] == 0xFE);

    printf("[PASS] test_legacy_protocol_100pct_preserved\n");
}

static void test_v2_framing_crc_and_coexistence(void)
{
    /* Verify 0xA5 and 0x5A are never valid legacy single-byte commands */
    uso_parser_t leg;
    memset(&leg, 0, sizeof(leg));
    assert(uso_parse(&leg, USO_V2_MAGIC_RX0, 0).kind == USO_INVALID);
    assert(uso_parse(&leg, USO_V2_MAGIC_RX1, 0).kind == USO_INVALID);

    /* Encode CMD_CONFIGURE and round-trip through incremental parser with noise prefix */
    uso_v2_config_t cfg;
    uso_v2_default_config(&cfg);
    cfg.carrier_freq_mhz = 3600u;
    cfg.pga_gain = 4u;
    cfg.adc_flags = USO_V2_ADC_FLAG_CHOP | USO_V2_ADC_FLAG_VBIAS;
    cfg.dsp_lpf_mode = USO_V2_LPF_BUTTER4;
    cfg.dsp_lpf_cutoff_dhz = 120u; /* 12.0 Hz */
    cfg.dsp_hpf_cutoff_mhz = 150u; /* 0.15 Hz */

    uint8_t payload[USO_V2_CFG_WIRE_LEN];
    uso_v2_pack_config(&cfg, payload);

    uint8_t wire[USO_V2_MAX_FRAME];
    unsigned wlen = uso_v2_encode_frame(
        USO_V2_MAGIC_RX0, USO_V2_MAGIC_RX1, 0u, USO_V2_CMD_CONFIGURE, 42u,
        payload, sizeof(payload), wire
    );
    assert(wlen == USO_V2_OVERHEAD + USO_V2_CFG_WIRE_LEN);
    assert(wlen < 64u); /* Guaranteed single USB FS bulk packet with zero ZLP */

    uso_v2_parser_t p;
    uso_v2_parser_init(&p, USO_V2_MAGIC_RX0, USO_V2_MAGIC_RX1);

    /* Spurious 0xA5 followed by legacy 0xAA must return USO_V2_PARSE_NOT_V2 so legacy handles 0xAA */
    assert(uso_v2_parser_feed(&p, 0xA5, 10, 250, NULL) == USO_V2_PARSE_NONE);
    assert(uso_v2_parser_feed(&p, 0xAA, 11, 250, NULL) == USO_V2_PARSE_NOT_V2);

    /* Feed valid V2 frame byte-by-byte */
    uso_v2_frame_t rx;
    memset(&rx, 0, sizeof(rx));
    uso_v2_parse_res_t last = USO_V2_PARSE_NONE;
    for (unsigned i = 0; i < wlen; ++i) {
        last = uso_v2_parser_feed(&p, wire[i], 20 + i, 250, &rx);
        if (i + 1 < wlen) assert(last == USO_V2_PARSE_NONE);
    }
    assert(last == USO_V2_PARSE_FRAME);
    assert(rx.version == USO_V2_PROTO_VERSION);
    assert(rx.msg_type == USO_V2_CMD_CONFIGURE);
    assert(rx.seq == 42u);

    uso_v2_config_t cfg_out;
    assert(uso_v2_unpack_config(rx.payload, rx.payload_len, &cfg_out));
    assert(cfg_out.carrier_freq_mhz == 3600u);
    assert(cfg_out.pga_gain == 4u);
    assert(cfg_out.adc_flags == (USO_V2_ADC_FLAG_CHOP | USO_V2_ADC_FLAG_VBIAS));
    assert(cfg_out.dsp_lpf_mode == USO_V2_LPF_BUTTER4);
    assert(cfg_out.dsp_lpf_cutoff_dhz == 120u);
    assert(cfg_out.dsp_hpf_cutoff_mhz == 150u);

    /* Corrupt 1 bit -> CRC must fail cleanly */
    wire[10] ^= 0x04u;
    for (unsigned i = 0; i < wlen; ++i) {
        last = uso_v2_parser_feed(&p, wire[i], 100 + i, 250, &rx);
    }
    assert(last == USO_V2_PARSE_BAD_CRC);
    assert(p.crc_errors == 1u);

    printf("[PASS] test_v2_framing_crc_and_coexistence\n");
}

static void test_v2_batch_streaming_packing(void)
{
    uso_v2_batch_t tx;
    memset(&tx, 0, sizeof(tx));
    tx.timestamp_ms = 0x12345678u;
    tx.first_sample_idx = 987654u;
    tx.effective_fs_dhz = 2000u; /* 200.0 Hz */
    tx.flags = USO_V2_BATCH_PGA_OVF | USO_V2_BATCH_HPF_ACTIVE;
    tx.pga_gain = 5u;
    tx.sample_count = USO_V2_MAX_BATCH_SAMPLES; /* 10 samples */
    for (uint8_t i = 0; i < tx.sample_count; ++i) {
        tx.samples[i] = (i % 2 == 0) ? (1000000 * (int32_t)(i + 1)) : (-500000 * (int32_t)(i + 1));
    }

    uint8_t wire[USO_V2_MAX_FRAME];
    unsigned wlen = uso_v2_encode_batch(1024u, &tx, wire);
    assert(wlen == 61u); /* 9 overhead + 12 hdr + 40 samples = 61 bytes < 64 USB FS packet! */

    uso_v2_parser_t hp;
    uso_v2_parser_init(&hp, USO_V2_MAGIC_TX0, USO_V2_MAGIC_TX1);
    uso_v2_frame_t frame;
    uso_v2_parse_res_t res = USO_V2_PARSE_NONE;
    for (unsigned i = 0; i < wlen; ++i) {
        res = uso_v2_parser_feed(&hp, wire[i], i, 250, &frame);
    }
    assert(res == USO_V2_PARSE_FRAME);
    assert(frame.msg_type == USO_V2_MSG_DATA_BATCH);
    assert(frame.seq == 1024u);

    uso_v2_batch_t rx;
    assert(uso_v2_unpack_batch(frame.payload, frame.payload_len, &rx));
    assert(rx.timestamp_ms == tx.timestamp_ms);
    assert(rx.first_sample_idx == tx.first_sample_idx);
    assert(rx.effective_fs_dhz == 2000u);
    assert(rx.flags == tx.flags);
    assert(rx.pga_gain == 5u);
    assert(rx.sample_count == 10u);
    for (uint8_t i = 0; i < 10u; ++i) {
        assert(rx.samples[i] == tx.samples[i]);
    }

    printf("[PASS] test_v2_batch_streaming_packing (61B frame < 64B USB FS MPS)\n");
}

static void test_adaptive_sample_rate_selection(void)
{
    uso_v2_config_t cfg;
    uso_v2_start_rsp_t resolved;
    uso_v2_default_config(&cfg);
    assert(uso_v2_resolve_config(&cfg, 7372800u, &resolved));
    assert(resolved.hw_fs_dhz == 4000u && resolved.effective_fs_dhz == 2000u);
    cfg.adc_flags |= USO_V2_ADC_FLAG_CHOP;
    assert(!uso_v2_resolve_config(&cfg, 7372800u, &resolved));
    uso_v2_default_config(&cfg);
    cfg.decimation = 17;
    assert(!uso_v2_resolve_config(&cfg, 7372800u, &resolved));
    cfg.decimation = 0; cfg.reserved = 1;
    assert(!uso_v2_resolve_config(&cfg, 7372800u, &resolved));
    cfg.reserved = 0; cfg.batch_size = 11;
    assert(!uso_v2_resolve_config(&cfg, 7372800u, &resolved));
    cfg.batch_size = 0; cfg.dsp_lpf_mode = USO_V2_LPF_LEGACY10;
    assert(!uso_v2_resolve_config(&cfg, 4608000u, &resolved));
    assert(uso_v2_resolve_config(&cfg, 7372800u, &resolved));
    assert(uso_v2_dr_nominal_dhz(USO_V2_DR_400, 7372800u, true) == 0u);
    assert(uso_v2_dr_nominal_dhz(USO_V2_DR_400, 7372800u, false) == 4000u);
    uint8_t dr = 0, dec = 0, batch = 0;
    uint16_t fs_dhz = 0;

    /* Internal 7.3728 MHz oscillator */
    uso_v2_select_adaptive_rate(16000u, 7372800u, false, &dr, &dec, &batch, &fs_dhz);
    assert(dr == USO_V2_DR_400 && dec == 1u && fs_dhz == 4000u && batch >= 6u && batch <= 10u);

    uso_v2_select_adaptive_rate(5000u, 7372800u, false, &dr, &dec, &batch, &fs_dhz);
    assert(dr == USO_V2_DR_400 && dec == 2u && fs_dhz == 2000u); /* 200 Hz (20 sps/half-period!) */

    uso_v2_select_adaptive_rate(2500u, 7372800u, false, &dr, &dec, &batch, &fs_dhz);
    assert(dr == USO_V2_DR_100 && dec == 1u && fs_dhz == 1000u); /* 100 Hz Sinc4 */

    uso_v2_select_adaptive_rate(1800u, 7372800u, false, &dr, &dec, &batch, &fs_dhz);
    assert(dr == USO_V2_DR_60 && dec == 1u && fs_dhz == 600u);   /* 60 Hz Sinc4 */

    /* External 4.608 MHz Rev P oscillator (0.625x scaling -> 400 becomes 250.0 Hz!) */
    uso_v2_select_adaptive_rate(5000u, 4608000u, false, &dr, &dec, &batch, &fs_dhz);
    assert(dr == USO_V2_DR_400 && dec == 1u && fs_dhz == 2500u);

    uso_v2_select_adaptive_rate(1800u, 4608000u, false, &dr, &dec, &batch, &fs_dhz);
    assert(dr == USO_V2_DR_100 && dec == 1u && fs_dhz == 625u);  /* 62.5 Hz > 46.8 Hz virtual */

    printf("[PASS] test_adaptive_sample_rate_selection (int 7.3728MHz + ext 4.608MHz)\n");
}

static void test_ads_decode_pga_vs_fatal_alarm(void)
{
    int32_t val = 0;
    uint8_t status = 0;
    bool ovf = false;

    /* Normal sample: status=0x40 (ADC1 new), value=0x01020304 */
    uint8_t ok_pkt[6] = { 0x40, 0x01, 0x02, 0x03, 0x04, (uint8_t)(0x9B + 1 + 2 + 3 + 4) };
    assert(ads_decode(ok_pkt, &val, &status) == ADS_OK && val == 0x01020304);
    assert(ads_decode_ex(ok_pkt, &val, &status, &ovf) == ADS_OK && !ovf && val == 0x01020304);

    /* Exercise every STATUS bit independently against TI Table 9-18. */
    for (unsigned bit = 0; bit < 6; ++bit) {
        uint8_t packet[6] = { (uint8_t)(0x40u | (1u << bit)), 0, 0, 0, 1, 0x9C };
        ads_result_t result = ads_decode_ex(packet, &val, &status, &ovf);
        if (bit == 0 || bit == 4) {
            assert(result == ADS_ALARM); /* RESET or REF_ALM */
        } else {
            assert(result == ADS_OK && val == 1);
            assert(ovf == (bit >= 1 && bit <= 3));
        }
    }
    uint8_t stale_pkt[6] = {0, 0, 0, 0, 1, 0x9C};
    assert(ads_decode_ex(stale_pkt, &val, &status, &ovf) == ADS_STALE);
    stale_pkt[0] = 0x40; stale_pkt[5] ^= 1;
    assert(ads_decode_ex(stale_pkt, &val, &status, &ovf) == ADS_CHECKSUM);

    printf("[PASS] test_ads_decode_pga_vs_fatal_alarm\n");
}

static void test_dsp_bypass_butter4_hpf_decim(void)
{
    uso_filter_t f;

    /* 1. Bypass mode (LPF=0, HPF=0, decim=1) must be bit-exact passthrough */
    assert(uso_filter_init_ex(&f, 400.0, USO_V2_LPF_BYPASS, 0.0, 0.0, 1u));
    int32_t out = 0;
    bool clipped = false;
    assert(uso_filter_step_ex(&f, 12345678, &out, &clipped) && out == 12345678 && !clipped);
    assert(uso_filter_step_ex(&f, -9876543, &out, &clipped) && out == -9876543 && !clipped);

    /* 2. Dynamic 4th-order Butterworth at Fs=200 Hz, fc=15 Hz: passes 5 Hz, attenuates 60 Hz > 35 dB */
    assert(uso_filter_init_ex(&f, 200.0, USO_V2_LPF_BUTTER4, 15.0, 0.0, 1u));
    double max_5hz = 0.0, max_60hz = 0.0;
    for (int i = 0; i < 600; ++i) {
        int32_t x = (int32_t)(1000000.0 * sin(2.0 * 3.14159265358979323846 * 5.0 * i / 200.0));
        uso_filter_step_ex(&f, x, &out, &clipped);
        if (i > 200 && fabs((double)out) > max_5hz) max_5hz = fabs((double)out);
    }
    assert(uso_filter_init_ex(&f, 200.0, USO_V2_LPF_BUTTER4, 15.0, 0.0, 1u));
    for (int i = 0; i < 600; ++i) {
        int32_t x = (int32_t)(1000000.0 * sin(2.0 * 3.14159265358979323846 * 60.0 * i / 200.0));
        uso_filter_step_ex(&f, x, &out, &clipped);
        if (i > 200 && fabs((double)out) > max_60hz) max_60hz = fabs((double)out);
    }
    assert(max_5hz > 950000.0 && max_5hz < 1050000.0);
    assert(max_60hz < 20000.0); /* > 34 dB attenuation at 4x cutoff */

    /* 3. Decimation by 2 averages pairs and emits half as many outputs */
    assert(uso_filter_init_ex(&f, 400.0, USO_V2_LPF_BYPASS, 0.0, 0.0, 2u));
    assert(!uso_filter_step_ex(&f, 1000, &out, &clipped));
    assert(uso_filter_step_ex(&f, 3000, &out, &clipped) && out == 2000);

    /* 4. Dynamic HPF at Fs=100 Hz blocks DC offset cleanly */
    assert(uso_filter_init_ex(&f, 100.0, USO_V2_LPF_BYPASS, 0.0, 0.5, 1u));
    for (int i = 0; i < 400; ++i) {
        uso_filter_step_ex(&f, 10000000, &out, &clipped);
    }
    assert(abs(out) < 50000); /* DC settled below 0.5% */

    printf("[PASS] test_dsp_bypass_butter4_hpf_decim (5Hz=%.0f, 60Hz=%.0f)\n", max_5hz, max_60hz);
}

int main(void)
{
    test_legacy_protocol_100pct_preserved();
    test_v2_framing_crc_and_coexistence();
    test_v2_batch_streaming_packing();
    test_adaptive_sample_rate_selection();
    test_ads_decode_pga_vs_fatal_alarm();
    test_dsp_bypass_butter4_hpf_decim();
    printf("ALL USO32 V2 EMBEDDED TESTS PASSED!\n");
    return 0;
}

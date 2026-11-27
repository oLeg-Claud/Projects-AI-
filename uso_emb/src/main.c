#include "ads1262.h"
#include "board.h"
#include "dsp.h"
#include "protocol.h"
#include "uso_proto_v2.h"
#include "usb_link.h"
#include "uso_config.h"
#include "boot_layout.h"
#include <string.h>

/* Inspect with SWD or V2 CMD_GET_DIAG. No diagnostic text is injected into binary streams. */
volatile struct {
    uint32_t samples, checksum_errors, adc_errors, stale, missed_drdy;
    uint32_t tx_overflows, rx_overflows, invalid_commands, timeouts;
    uint32_t dropped_samples, v2_crc_errors;
    uint8_t adc_status, fault, running, v2_mode;
} uso_stats;

static uint8_t rx[USO_RX_CAPACITY];
static volatile uint32_t rx_head, rx_tail, drdy_count;
static volatile bool disconnected, rx_overflow;

typedef struct { uint8_t data[64], size; bool is_data; } frame_t;
static frame_t tx[USO_TX_FRAMES];
static unsigned tx_head, tx_tail;

static uso_parser_t parser;
static uso_v2_parser_t v2_parser;
static uso_filter_t filter;

static bool ready, running, hp_enabled, bits32, v2_mode;
static uint32_t last_sample, adc_timeout_ms = USO_ADC_TIMEOUT_MS;
static bool boot_pending;

/* Active V2 state */
static uso_v2_config_t    v2_cfg;
static uso_v2_start_rsp_t v2_active;
static uso_v2_batch_t     v2_batch;
static uint16_t           v2_tx_seq;
static uint32_t           v2_sample_idx;
static bool               v2_gap_pending;
static bool               v2_decim_overflow;
static uint32_t           v2_batch_start_ms;

static const uint8_t info[] = "\xa1" __DATE__ " " __TIME__ " USO32_App_v3.1_DualV2";

void app_receive(const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t next = (rx_head + 1u) % USO_RX_CAPACITY;
        if (next == rx_tail) { rx_overflow = true; ++uso_stats.rx_overflows; return; }
        rx[rx_head] = b[i];
        __DMB();
        rx_head = next;
    }
}

void app_disconnect(void) { disconnected = true; }

void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    if (pin == GPIO_PIN_6) ++drdy_count;
}

static void stop_acquisition(void)
{
    board_adc_irq(false);
    drdy_count = 0;
    running = false;
    uso_stats.running = 0;
    v2_batch.sample_count = 0u;
    if (!ads_stop()) {
        ready = false;
        uso_stats.fault = 1;
        board_reset(false);
    }
    /* Drop queued data frames while keeping command replies intact. */
    unsigned write = tx_tail;
    for (unsigned read = tx_tail; read != tx_head; read = (read + 1u) % USO_TX_FRAMES) {
        if (!tx[read].is_data) {
            tx[write] = tx[read];
            write = (write + 1u) % USO_TX_FRAMES;
        }
    }
    tx_head = write;
}

static void fault(void)
{
    stop_acquisition();
    ready = false;
    uso_stats.fault = 1;
    board_reset(false);
}

static bool enqueue_ex(const uint8_t *data, unsigned n, bool is_data)
{
    if (n == 0u || n > sizeof(tx[0].data)) return false;
    unsigned next = (tx_head + 1u) % USO_TX_FRAMES;
    if (next == tx_tail) {
        ++uso_stats.tx_overflows;
        if (v2_mode && is_data && (v2_cfg.stream_flags & USO_V2_STREAM_AUTO_RECOVER)) {
            /* In V2 auto-recover mode, drop this batch and flag GAP on the next one
             * instead of terminating the session. */
            uso_stats.dropped_samples += v2_batch.sample_count ? v2_batch.sample_count : 1u;
            v2_gap_pending = true;
            return false;
        }
        /* Legacy behavior: end acquisition and discard queued samples, keep replies. */
        stop_acquisition();
        if (is_data) return false;
        next = (tx_head + 1u) % USO_TX_FRAMES;
        if (next == tx_tail) return false;
    }
    memcpy(tx[tx_head].data, data, n);
    tx[tx_head].size = (uint8_t)n;
    tx[tx_head].is_data = is_data;
    tx_head = next;
    return true;
}

static void reply_legacy_ack(void)
{
    const uint8_t ack = 0x55;
    (void)enqueue_ex(&ack, 1u, false);
}

static void send_v2_response(uint8_t msg_type, uint16_t seq, const uint8_t *payload, uint8_t len)
{
    uint8_t wire[USO_V2_MAX_FRAME];
    unsigned n = uso_v2_encode_frame(
        USO_V2_MAGIC_TX0, USO_V2_MAGIC_TX1, 0u, msg_type, seq, payload, len, wire
    );
    if (n > 0u) {
        (void)enqueue_ex(wire, n, false);
    }
}

static void send_v2_ack(uint16_t seq, uint8_t cmd_echo, uint8_t status)
{
    uint8_t p[2] = { cmd_echo, status };
    send_v2_response(USO_V2_RSP_ACK, seq, p, sizeof(p));
}

static void flush_v2_batch(void)
{
    if (v2_batch.sample_count == 0u) return;
    uint8_t wire[USO_V2_MAX_FRAME];
    unsigned n = uso_v2_encode_batch(v2_tx_seq++, &v2_batch, wire);
    if (n > 0u) {
        (void)enqueue_ex(wire, n, true);
    }
    v2_batch.sample_count = 0u;
    v2_batch.flags = 0u;
}

static void mark_v2_gap(void)
{
    // Preserve the valid prefix, then start a new segment after the missing ADC data.
    flush_v2_batch();
    v2_gap_pending = true;
    memset(filter.state, 0, sizeof(filter.state));
    filter.hp_x = filter.hp_y = 0;
    filter.decim_count = 0;
    filter.decim_accum = 0;
    v2_decim_overflow = false;
}

static bool resolve_v2_stream(const uso_v2_config_t *cfg, uso_v2_start_rsp_t *out)
{
    return uso_v2_resolve_config(cfg, ads_get_clock_hz(), out);
}

static bool start_v2_acquisition(void)
{
    if (!ready) return false;
    stop_acquisition();
    if (!ready) return false;

    uso_v2_start_rsp_t resolved;
    if (!resolve_v2_stream(&v2_cfg, &resolved)) return false;

    double hw_fs_hz = (double)resolved.hw_fs_dhz * 0.1;
    double lpf_hz = (v2_cfg.dsp_lpf_cutoff_dhz > 0u)
                        ? ((double)v2_cfg.dsp_lpf_cutoff_dhz * 0.1)
                        : ((double)v2_cfg.carrier_freq_mhz * 0.0025); /* ~2.5 * carrier */
    double hpf_hz = (double)v2_cfg.dsp_hpf_cutoff_mhz * 0.001;

    if (!uso_filter_init_ex(&filter, hw_fs_hz, v2_cfg.dsp_lpf_mode, lpf_hz, hpf_hz, resolved.decimation)) {
        return false;
    }

    ads_config_t acfg;
    acfg.gain       = resolved.pga_gain;
    acfg.pga_bypass = (resolved.adc_flags & USO_V2_ADC_FLAG_PGA_BYP) != 0u;
    acfg.dr_code    = resolved.dr_code;
    acfg.filter     = resolved.adc_filter;
    acfg.chop       = (resolved.adc_flags & USO_V2_ADC_FLAG_CHOP) != 0u;
    acfg.vbias      = (resolved.adc_flags & USO_V2_ADC_FLAG_VBIAS) != 0u;
    acfg.inpmux     = resolved.inpmux;

    if (!ads_start_ex(&acfg)) {
        fault();
        return false;
    }

    v2_active = resolved;
    v2_mode = true;
    uso_stats.v2_mode = 1;
    v2_batch.sample_count = 0u;
    v2_batch.flags = 0u;
    // Keep the output index monotonic across acquisition restarts. The first
    // batch explicitly starts a new signal segment even if no index is skipped.
    v2_gap_pending = true;
    v2_decim_overflow = false;

    /* Scale ADC watchdog timeout with sample period so 20/50/60 SPS never spuriously trip. */
    uint32_t period_ms = (resolved.hw_fs_dhz > 0u) ? (10000u / resolved.hw_fs_dhz) : 10u;
    adc_timeout_ms = (period_ms * 6u > USO_ADC_TIMEOUT_MS) ? (period_ms * 6u) : USO_ADC_TIMEOUT_MS;

    last_sample = HAL_GetTick();
    v2_batch_start_ms = last_sample;
    running = true;
    uso_stats.running = 1;
    drdy_count = 0;
    board_adc_irq(true);
    return true;
}

static void handle_v2(const uso_v2_frame_t *f)
{
    switch (f->msg_type) {
    case USO_V2_CMD_HELLO: {
        uso_v2_hello_rsp_t rsp;
        memset(&rsp, 0, sizeof(rsp));
        rsp.proto_version = USO_V2_PROTO_VERSION;
        rsp.device_id     = (uint8_t)USO_DEVICE_NUMBER;
        rsp.fw_major      = USO_FW_MAJOR;
        rsp.fw_minor      = USO_FW_MINOR;
        rsp.fw_patch      = USO_FW_PATCH;
        rsp.adc_id        = ads_get_chip_id();
        rsp.ready         = ready ? 1u : 0u;
        rsp.fault         = uso_stats.fault;
        rsp.capabilities  = USO_V2_CAP_ADAPTIVE_RATE |
                            USO_V2_CAP_HW_FILTERS |
                            USO_V2_CAP_RECALIBRATE |
                            USO_V2_CAP_SOFT_RESET_ADC |
                            USO_V2_CAP_DIAG_MUX |
                            USO_V2_CAP_LEGACY_FALLBACK;
        rsp.adc_clock_hz  = ads_get_clock_hz();
        uint32_t uid[3]   = { HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2() };
        memcpy(rsp.mcu_uid, uid, 12u);
        uint8_t buf[USO_V2_HELLO_RSP_WIRE_LEN];
        uso_v2_pack_hello_rsp(&rsp, buf);
        send_v2_response(USO_V2_RSP_HELLO, f->seq, buf, sizeof(buf));
        break;
    }

    case USO_V2_CMD_CONFIGURE: {
        uso_v2_config_t next_cfg;
        if (!uso_v2_unpack_config(f->payload, f->payload_len, &next_cfg)) {
            send_v2_ack(f->seq, f->msg_type, USO_V2_ERR_BAD_LEN);
            break;
        }
        uso_v2_start_rsp_t check;
        if (!resolve_v2_stream(&next_cfg, &check)) {
            send_v2_ack(f->seq, f->msg_type, USO_V2_ERR_BAD_PARAM);
            break;
        }
        v2_cfg = next_cfg;
        if (running) {
            if (!start_v2_acquisition()) {
                send_v2_ack(f->seq, f->msg_type, USO_V2_ERR_ADC_FAULT);
                break;
            }
        }
        send_v2_ack(f->seq, f->msg_type, USO_V2_OK);
        break;
    }

    case USO_V2_CMD_START: {
        uso_v2_config_t next_cfg = v2_cfg;
        uso_v2_start_rsp_t check;
        uint8_t status = USO_V2_OK;
        if (f->payload_len != 0u && f->payload_len != USO_V2_CFG_WIRE_LEN)
            status = USO_V2_ERR_BAD_LEN;
        else if (f->payload_len && !uso_v2_unpack_config(f->payload, f->payload_len, &next_cfg))
            status = USO_V2_ERR_BAD_LEN;
        else if (!resolve_v2_stream(&next_cfg, &check)) status = USO_V2_ERR_BAD_PARAM;
        else if (!ready) status = USO_V2_ERR_ADC_FAULT;
        if (status != USO_V2_OK) {
            uso_v2_start_rsp_t err = {0};
            err.status = status;
            uint8_t buf[USO_V2_START_RSP_WIRE_LEN];
            uso_v2_pack_start_rsp(&err, buf);
            send_v2_response(USO_V2_RSP_START, f->seq, buf, sizeof(buf));
            break;
        }
        v2_cfg = next_cfg;
        if (!start_v2_acquisition()) {
            uso_v2_start_rsp_t err;
            memset(&err, 0, sizeof(err));
            err.status = USO_V2_ERR_ADC_FAULT;
            uint8_t buf[USO_V2_START_RSP_WIRE_LEN];
            uso_v2_pack_start_rsp(&err, buf);
            send_v2_response(USO_V2_RSP_START, f->seq, buf, sizeof(buf));
            break;
        }
        uint8_t buf[USO_V2_START_RSP_WIRE_LEN];
        uso_v2_pack_start_rsp(&v2_active, buf);
        send_v2_response(USO_V2_RSP_START, f->seq, buf, sizeof(buf));
        break;
    }

    case USO_V2_CMD_STOP:
        stop_acquisition();
        v2_mode = false;
        uso_stats.v2_mode = 0;
        send_v2_ack(f->seq, f->msg_type, USO_V2_OK);
        break;

    case USO_V2_CMD_SET_LIVE: {
        uso_v2_live_t live;
        if (!uso_v2_unpack_live(f->payload, f->payload_len, &live)) {
            send_v2_ack(f->seq, f->msg_type, USO_V2_ERR_BAD_LEN);
            break;
        }
        if (!running || !v2_mode) {
            send_v2_ack(f->seq, f->msg_type, USO_V2_ERR_BUSY);
            break;
        }
        if (!live.mask || (live.mask & ~0x0Fu) ||
            ((live.mask & USO_V2_LIVE_MASK_GAIN) && live.pga_gain > 5u) ||
            ((live.mask & USO_V2_LIVE_MASK_BATCH) && (!live.batch_size || live.batch_size > USO_V2_MAX_BATCH_SAMPLES))) {
            send_v2_ack(f->seq, f->msg_type, USO_V2_ERR_BAD_PARAM);
            break;
        }
        uso_v2_config_t next_cfg = v2_cfg;
        if (live.mask & USO_V2_LIVE_MASK_GAIN) next_cfg.pga_gain = live.pga_gain;
        if (live.mask & USO_V2_LIVE_MASK_HPF) next_cfg.dsp_hpf_cutoff_mhz = live.dsp_hpf_cutoff_mhz;
        if (live.mask & USO_V2_LIVE_MASK_LPF) {
            next_cfg.dsp_lpf_mode = live.dsp_lpf_mode;
            next_cfg.dsp_lpf_cutoff_dhz = live.dsp_lpf_cutoff_dhz;
        }
        if (live.mask & USO_V2_LIVE_MASK_BATCH) next_cfg.batch_size = live.batch_size;
        uso_v2_start_rsp_t check;
        if (!resolve_v2_stream(&next_cfg, &check)) {
            send_v2_ack(f->seq, f->msg_type, USO_V2_ERR_BAD_PARAM);
            break;
        }
        v2_cfg = next_cfg;
        if (live.mask & (USO_V2_LIVE_MASK_GAIN | USO_V2_LIVE_MASK_HPF | USO_V2_LIVE_MASK_LPF)) {
            // An ADC/filter restart is a discontinuity, not a seamless update.
            if (!start_v2_acquisition()) {
                send_v2_ack(f->seq, f->msg_type, USO_V2_ERR_ADC_FAULT);
                break;
            }
        } else {
            flush_v2_batch();
            v2_active.batch_size = check.batch_size;
        }
        send_v2_ack(f->seq, f->msg_type, USO_V2_OK);
        break;
    }

    case USO_V2_CMD_GET_DIAG: {
        uso_v2_diag_rsp_t diag;
        diag.uptime_ms        = HAL_GetTick();
        diag.samples          = uso_stats.samples;
        diag.dropped_samples  = uso_stats.dropped_samples;
        diag.missed_drdy      = uso_stats.missed_drdy;
        diag.checksum_errors  = uso_stats.checksum_errors;
        diag.adc_errors       = uso_stats.adc_errors;
        diag.stale_count      = uso_stats.stale;
        diag.tx_overflows     = uso_stats.tx_overflows;
        diag.rx_overflows     = uso_stats.rx_overflows;
        diag.v2_crc_errors    = v2_parser.crc_errors;
        diag.effective_fs_dhz = running ? (v2_mode ? v2_active.effective_fs_dhz : (uint16_t)(USO_SAMPLE_RATE_HZ * 10u)) : 0u;
        diag.adc_status       = uso_stats.adc_status;
        diag.state_flags      = (uint8_t)((ready ? 1u : 0u) |
                                          (running ? 2u : 0u) |
                                          (uso_stats.fault ? 4u : 0u) |
                                          (v2_mode ? 8u : 0u));
        uint8_t buf[USO_V2_DIAG_RSP_WIRE_LEN];
        uso_v2_pack_diag_rsp(&diag, buf);
        send_v2_response(USO_V2_RSP_DIAG, f->seq, buf, sizeof(buf));
        break;
    }

    case USO_V2_CMD_RECALIBRATE: {
        bool was_running = running;
        stop_acquisition();
        if (!ready) {
            uint8_t err = USO_V2_ERR_ADC_FAULT;
            send_v2_response(USO_V2_RSP_CAL, f->seq, &err, 1u);
            break;
        }
        uint8_t dr = v2_active.dr_code ? v2_active.dr_code : USO_V2_DR_400;
        uint8_t flt = v2_active.adc_filter <= USO_V2_FLT_FIR ? v2_active.adc_filter : USO_V2_FLT_SINC4;
        bool chop = (v2_cfg.adc_flags & USO_V2_ADC_FLAG_CHOP) != 0u;
        int32_t offs[6] = {0};
        if (!ads_recalibrate(dr, flt, chop, offs)) {
            uint8_t err = USO_V2_ERR_CAL_FAIL;
            send_v2_response(USO_V2_RSP_CAL, f->seq, &err, 1u);
            break;
        }
        if (was_running && !start_v2_acquisition()) {
            uint8_t err = USO_V2_ERR_ADC_FAULT;
            send_v2_response(USO_V2_RSP_CAL, f->seq, &err, 1u);
            break;
        }
        uint8_t payload[25];
        payload[0] = USO_V2_OK;
        for (unsigned g = 0; g < 6u; ++g) {
            uint32_t raw;
            memcpy(&raw, &offs[g], sizeof(raw));
            payload[1u + g * 4u + 0u] = (uint8_t)(raw & 0xFFu);
            payload[1u + g * 4u + 1u] = (uint8_t)((raw >> 8) & 0xFFu);
            payload[1u + g * 4u + 2u] = (uint8_t)((raw >> 16) & 0xFFu);
            payload[1u + g * 4u + 3u] = (uint8_t)((raw >> 24) & 0xFFu);
        }
        send_v2_response(USO_V2_RSP_CAL, f->seq, payload, sizeof(payload));
        break;
    }

    case USO_V2_CMD_RESET_ADC: {
        stop_acquisition();
        ready = ads_init();
        uso_stats.fault = ready ? 0u : 1u;
        if (!ready) board_reset(false);
        send_v2_ack(f->seq, f->msg_type, ready ? USO_V2_OK : USO_V2_ERR_ADC_FAULT);
        break;
    }

    default:
        send_v2_ack(f->seq, f->msg_type, USO_V2_ERR_UNKNOWN_CMD);
        break;
    }
}

static void handle_legacy(uso_event_t e)
{
    if (!uso_event_allowed(e.kind, running)) return;
    switch (e.kind) {
    case USO_NUMBER: {
        uint8_t frame[5];
        unsigned n = uso_pack_number(frame);
        (void)enqueue_ex(frame, n, false);
        break;
    }
    case USO_INFO:
        if (!running) (void)enqueue_ex(info, sizeof(info), false);
        break;
    case USO_BOOT:
        if (!running) {
            const uint8_t ack[] = { 0x08, 0xAA };
            if (enqueue_ex(ack, sizeof(ack), false)) boot_pending = true;
        }
        break;
    case USO_READY:
        if (ready) reply_legacy_ack();
        break;
    case USO_STOP:
        stop_acquisition();
        v2_mode = false;
        uso_stats.v2_mode = 0;
        reply_legacy_ack();
        break;
    case USO_HP_ON:
    case USO_HP_OFF:
        hp_enabled = (e.kind == USO_HP_ON);
        uso_filter_hp(&filter, hp_enabled);
        break;
    case USO_START:
        if (!ready) break;
        stop_acquisition();
        v2_mode = false;
        uso_stats.v2_mode = 0;
        adc_timeout_ms = USO_ADC_TIMEOUT_MS;
        if (!ready || !uso_filter_init(&filter, e.freq, hp_enabled) || !ads_start(e.gain)) {
            fault();
            break;
        }
        bits32 = e.bits32;
        last_sample = HAL_GetTick();
        running = true;
        uso_stats.running = 1;
        drdy_count = 0;
        board_adc_irq(true);
        break;
    case USO_INVALID:
        ++uso_stats.invalid_commands;
        break;
    default:
        break;
    }
}

static void process_rx_byte(uint8_t byte, uint32_t now)
{
    /* Legacy multi-byte commands (START pair or BOOT key) take priority while pending */
    if (parser.pending) {
        handle_legacy(uso_parse(&parser, byte, now));
        return;
    }

    /* Check V2 framed protocol magic (0xA5, 0x5A) without interfering with legacy bytes */
    if (uso_v2_parser_active(&v2_parser) || byte == USO_V2_MAGIC_RX0) {
        uso_v2_frame_t frame;
        uso_v2_parse_res_t res = uso_v2_parser_feed(&v2_parser, byte, now, USO_V2_TIMEOUT_MS, &frame);
        if (res == USO_V2_PARSE_FRAME) {
            handle_v2(&frame);
            return;
        }
        if (res == USO_V2_PARSE_BAD_CRC) {
            ++uso_stats.v2_crc_errors;
            return;
        }
        if (res == USO_V2_PARSE_NONE) {
            return;
        }
        /* USO_V2_PARSE_NOT_V2: spurious 0xA5 followed by a non-0x5A byte -> pass byte to legacy */
    }

    handle_legacy(uso_parse(&parser, byte, now));
}

int main(void)
{
    board_init();
    uso_v2_default_config(&v2_cfg);
    uso_v2_parser_init(&v2_parser, USO_V2_MAGIC_RX0, USO_V2_MAGIC_RX1);

    ready = ads_init();
    if (!ready) {
        uso_stats.fault = 1;
        board_reset(false);
    }
    usb_link_init();

    for (;;) {
        uint32_t now = HAL_GetTick();
        if (disconnected || rx_overflow) {
            uint32_t mask = __get_PRIMASK();
            __disable_irq();
            disconnected = false;
            rx_overflow = false;
            rx_tail = rx_head;
            __set_PRIMASK(mask);
            parser.pending = false;
            uso_v2_parser_reset(&v2_parser);
            boot_pending = false;
            tx_head = tx_tail = 0;
            v2_mode = false;
            uso_stats.v2_mode = 0;
            /* A transport overflow invalidates the session, not the ADC. */
            stop_acquisition();
        }

        /* Bound work per iteration so a host command flood cannot starve ADC. */
        for (unsigned budget = 0; budget < 32u && rx_tail != rx_head && !boot_pending; ++budget) {
            uint8_t byte = rx[rx_tail];
            __DMB();
            rx_tail = (rx_tail + 1u) % USO_RX_CAPACITY;
            process_rx_byte(byte, HAL_GetTick());
        }
        uso_parse_expire(&parser, HAL_GetTick());
        uso_v2_parser_expire(&v2_parser, HAL_GetTick(), USO_V2_TIMEOUT_MS);

        if (running) {
            uint32_t mask = __get_PRIMASK();
            __disable_irq();
            uint32_t pending = drdy_count;
            drdy_count = 0;
            __set_PRIMASK(mask);

            if (pending > 1u) {
                uso_stats.missed_drdy += pending - 1u;
                if (v2_mode && (v2_cfg.stream_flags & USO_V2_STREAM_AUTO_RECOVER)) {
                    uso_stats.dropped_samples += pending - 1u;
                    mark_v2_gap();
                } else {
                    fault();
                    pending = 0u;
                }
            }

            if (pending >= 1u && running) {
                int32_t sample = 0;
                uint8_t status = 0;
                bool pga_ovf = false;
                ads_result_t result = ads_read_ex(&sample, &status, &pga_ovf);
                uso_stats.adc_status = status;

                if (result == ADS_OK) {
                    uint32_t ts = HAL_GetTick();
                    last_sample = ts;
                    ++uso_stats.samples;

                    if (v2_mode) {
                        int32_t out_s = 0;
                        bool clipped = false;
                        v2_decim_overflow = v2_decim_overflow || pga_ovf;
                        if (uso_filter_step_ex(&filter, sample, &out_s, &clipped)) {
                            if (v2_batch.sample_count == 0u) {
                                v2_batch.first_sample_idx = v2_sample_idx;
                                v2_batch.effective_fs_dhz = v2_active.effective_fs_dhz;
                                v2_batch.pga_gain = v2_active.pga_gain;
                                v2_batch.flags = v2_gap_pending ? USO_V2_BATCH_GAP_DROPPED : 0u;
                                v2_gap_pending = false;
                                if (filter.hp) v2_batch.flags |= USO_V2_BATCH_HPF_ACTIVE;
                                if (filter.sos_count > 0u) v2_batch.flags |= USO_V2_BATCH_LPF_ACTIVE;
                                if (v2_active.adc_flags & USO_V2_ADC_FLAG_CHOP) v2_batch.flags |= USO_V2_BATCH_CHOP_ACTIVE;
                                v2_batch_start_ms = ts;
                            }
                            if (v2_decim_overflow || clipped) {
                                v2_batch.flags |= USO_V2_BATCH_PGA_OVF;
                            }
                            v2_decim_overflow = false;
                            v2_batch.timestamp_ms = ts; // time of last sample, not USB flush
                            v2_batch.samples[v2_batch.sample_count++] = out_s;
                            ++v2_sample_idx;
                            if (v2_batch.sample_count >= v2_active.batch_size) {
                                flush_v2_batch();
                            }
                        }
                    } else {
                        /* 100% Legacy 5/6-byte frame mode (emits 0xFE on PGA overrange) */
                        uint8_t frame[6];
                        int32_t filtered = uso_filter_step(&filter, sample);
                        unsigned n = uso_pack_sample_ex(filtered, bits32, pga_ovf, frame);
                        (void)enqueue_ex(frame, n, true);
                    }
                } else {
                    if (result == ADS_CHECKSUM) ++uso_stats.checksum_errors;
                    else if (result == ADS_STALE) ++uso_stats.stale;
                    else ++uso_stats.adc_errors;

                    if (v2_mode && result != ADS_ALARM && (v2_cfg.stream_flags & USO_V2_STREAM_AUTO_RECOVER)) {
                        mark_v2_gap();
                        ++uso_stats.dropped_samples;
                    } else {
                        fault();
                    }
                }
            } else if ((uint32_t)(HAL_GetTick() - last_sample) >= adc_timeout_ms) {
                ++uso_stats.timeouts;
                fault();
            }

            /* Bound V2 batch latency (flush partial batch if > 40 ms elapsed) */
            if (v2_mode && v2_batch.sample_count > 0u &&
                (uint32_t)(HAL_GetTick() - v2_batch_start_ms) >= 40u) {
                flush_v2_batch();
            }
        }

        if (tx_tail != tx_head && usb_link_send(tx[tx_tail].data, tx[tx_tail].size)) {
            tx_tail = (tx_tail + 1u) % USO_TX_FRAMES;
        }

        if (boot_pending && tx_tail == tx_head && usb_link_idle()) {
            board_adc_irq(false);
            board_reset(false);
            usb_link_stop();
            HAL_Delay(100);
            volatile uint32_t *mail = (volatile uint32_t *)BL_MAILBOX_ADDR;
            mail[0] = BL_REQUEST_MAGIC;
            mail[1] = ~BL_REQUEST_MAGIC;
            __DSB();
            NVIC_SystemReset();
        }

        board_led(uso_stats.fault ? ((now / 100u) & 1u) : running ? ((now / 500u) & 1u) : true);
    }
}

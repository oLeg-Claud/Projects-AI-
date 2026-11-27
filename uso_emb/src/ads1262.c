#ifndef USO_BOOTLOADER

#include "ads1262.h"
#include "board.h"
#include "uso_config.h"
#include <string.h>

enum { POWER = 1, INTERFACE = 2, MODE0 = 3, MODE1 = 4, MODE2 = 5, INPMUX = 6, OFCAL0 = 7 };
enum { START1 = 0x08, STOP1 = 0x0A, RDATA1 = 0x12, SFOCAL1 = 0x19 };

static uint8_t offsets[6][3];
static uint8_t chip_id;
static ads_config_t active_cfg;
static bool ext_clk_seen;

static bool transfer(uint8_t *tx, uint8_t *rx, uint16_t size)
{
    return HAL_SPI_TransmitReceive(&hspi3, tx, rx, size, 2) == HAL_OK;
}

static void select_adc(void) { board_cs(false); board_delay_us(1); }
static void release_adc(void) { board_delay_us(1); board_cs(true); board_delay_us(1); }

static bool command(uint8_t cmd)
{
    uint8_t rx;
    select_adc();
    bool ok = transfer(&cmd, &rx, 1);
    release_adc();
    return ok;
}

static bool read_regs(uint8_t addr, uint8_t *out, uint8_t n)
{
    uint8_t tx[23] = {0}, rx[23] = {0};
    if (!n || n > 21) return false;
    tx[0] = (uint8_t)(0x20u | addr);
    tx[1] = (uint8_t)(n - 1u);
    select_adc();
    bool ok = transfer(tx, rx, (uint16_t)(n + 2u));
    release_adc();
    memcpy(out, rx + 2, n);
    return ok;
}

static bool write_regs(uint8_t addr, const uint8_t *data, uint8_t n)
{
    uint8_t tx[23] = {0}, rx[23];
    if (!n || n > 21) return false;
    tx[0] = (uint8_t)(0x40u | addr);
    tx[1] = (uint8_t)(n - 1u);
    memcpy(tx + 2, data, n);
    select_adc();
    bool ok = transfer(tx, rx, (uint16_t)(n + 2u));
    release_adc();
    return ok;
}

static bool set_reg(uint8_t addr, uint8_t data)
{
    uint8_t actual = 0;
    return write_regs(addr, &data, 1) && read_regs(addr, &actual, 1) && actual == data;
}

void ads_default_config(ads_config_t *cfg)
{
    if (!cfg) return;
    cfg->gain       = 0u;
    cfg->pga_bypass = false;
    cfg->dr_code    = 0x08u; /* 400 SPS nominal */
    cfg->filter     = 3u;    /* Sinc4 */
    cfg->chop       = false;
    cfg->vbias      = true;
    cfg->inpmux     = 0x89u; /* AIN8 / AIN9 seismic transformer */
}

bool ads_stop(void) { return command(STOP1); }

static bool calibrate_all_gains(uint8_t dr_code, uint8_t filter, bool chop, int32_t out_offsets[6])
{
    if (dr_code < 0x04u || dr_code > 0x0Bu) return false;
    if (filter > 4u) return false;
    if (filter == 4u && dr_code > 0x04u) filter = 3u; /* FIR valid only <= 20 SPS */

    uint8_t mode0 = chop ? 0x10u : 0x00u;
    uint8_t mode1 = (uint8_t)((filter & 0x07u) << 5);
    if (!ads_stop() || !set_reg(MODE0, mode0) || !set_reg(MODE1, mode1)) return false;

    for (uint8_t gain = 0; gain < 6; ++gain) {
        uint8_t mode2 = (uint8_t)((gain << 4) | (dr_code & 0x0Fu));
        if (!set_reg(MODE2, mode2) || !set_reg(INPMUX, 0xFFu) || !command(START1)) return false;
        uint8_t cmd = SFOCAL1, rx;
        select_adc();
        bool ok = transfer(&cmd, &rx, 1);
        /* TI SBAS661C 9.4.9.8: keep CS and SCLK low throughout calibration. */
        HAL_Delay(1);
        uint32_t started = HAL_GetTick();
        while (ok && !board_drdy()) {
            if ((uint32_t)(HAL_GetTick() - started) >= 1500u) ok = false;
        }
        release_adc();
        if (!ok || !ads_stop() || !read_regs(OFCAL0, offsets[gain], 3)) return false;
        if (out_offsets) {
            int32_t v = ((int32_t)offsets[gain][2] << 16) |
                        ((int32_t)offsets[gain][1] << 8) |
                        (int32_t)offsets[gain][0];
            if (v & 0x00800000) v |= (int32_t)0xFF000000u;
            out_offsets[gain] = v;
        }
    }
    return true;
}

bool ads_init(void)
{
    ads_default_config(&active_cfg);
    ext_clk_seen = false;

    /* START stays low: only START1/STOP1 commands control conversion. */
    board_reset(false); HAL_Delay(10);
    board_reset(true); HAL_Delay(25);

    uint8_t id = 0xFFu;
    if (!read_regs(0, &id, 1) || (id >> 5) != 0u) return false;
    chip_id = id;

    /* Clear RESET flag, keep internal reference; enable VBIAS; CHOP off for nominal 400 SPS. */
    const uint8_t regs[20] = {
        0x03, 0x05, 0x00, 0x60, 0x08, 0x89,
        0, 0, 0, 0, 0, 0x40, 0xBB, 0, 0, 0, 0, 0, 0, 0
    };
    uint8_t verify[20];
    if (!ads_stop() || !write_regs(1, regs, 20) || !read_regs(1, verify, 20) ||
        memcmp(regs, verify, 20) != 0) {
        return false;
    }
    HAL_Delay(50);

    if (!calibrate_all_gains(0x08u, 3u, false, NULL)) return false;
    return set_reg(INPMUX, 0x89u);
}

bool ads_start(uint8_t gain)
{
    ads_config_t cfg;
    ads_default_config(&cfg);
    cfg.gain = gain;
    return ads_start_ex(&cfg);
}

bool ads_start_ex(const ads_config_t *cfg)
{
    if (!cfg || cfg->gain > 5u) return false;
    if (cfg->dr_code < 0x04u || cfg->dr_code > 0x0Bu) return false;
    if (cfg->filter > 4u) return false;

    uint8_t eff_filter = cfg->filter;
    if (eff_filter == 4u && cfg->dr_code > 0x04u) {
        eff_filter = 3u; /* FIR only supported at <= 20 SPS */
    }

    uint8_t power = (uint8_t)(0x01u | (cfg->vbias ? 0x02u : 0x00u));
    uint8_t mode0 = cfg->chop ? 0x10u : 0x00u;
    uint8_t mode1 = (uint8_t)((eff_filter & 0x07u) << 5);
    uint8_t mode2 = (uint8_t)((cfg->pga_bypass ? 0x80u : 0x00u) |
                              ((cfg->gain & 0x07u) << 4) |
                              (cfg->dr_code & 0x0Fu));
    uint8_t check[3];

    if (!ads_stop() ||
        !set_reg(POWER, power) ||
        !set_reg(MODE0, mode0) ||
        !set_reg(MODE1, mode1) ||
        !set_reg(MODE2, mode2) ||
        !set_reg(INPMUX, cfg->inpmux) ||
        !write_regs(OFCAL0, offsets[cfg->gain], 3) ||
        !read_regs(OFCAL0, check, 3) ||
        memcmp(check, offsets[cfg->gain], 3) != 0 ||
        !command(START1)) {
        return false;
    }

    active_cfg = *cfg;
    active_cfg.filter = eff_filter;
    return true;
}

bool ads_set_gain_live(uint8_t gain, bool pga_bypass)
{
    if (gain > 5u) return false;
    uint8_t mode2 = (uint8_t)((pga_bypass ? 0x80u : 0x00u) |
                              ((gain & 0x07u) << 4) |
                              (active_cfg.dr_code & 0x0Fu));
    uint8_t check[3];
    if (!ads_stop() ||
        !set_reg(MODE2, mode2) ||
        !write_regs(OFCAL0, offsets[gain], 3) ||
        !read_regs(OFCAL0, check, 3) ||
        memcmp(check, offsets[gain], 3) != 0 ||
        !command(START1)) {
        return false;
    }
    active_cfg.gain = gain;
    active_cfg.pga_bypass = pga_bypass;
    return true;
}

bool ads_recalibrate(uint8_t dr_code, uint8_t filter, bool chop, int32_t out_offsets[6])
{
    if (!calibrate_all_gains(dr_code, filter, chop, out_offsets)) return false;
    return set_reg(INPMUX, active_cfg.inpmux);
}

uint8_t ads_get_chip_id(void) { return chip_id; }

uint32_t ads_get_clock_hz(void)
{
    /* If ADC STATUS bit 7 (EXTCLK) was observed, Rev P external oscillator is 4.608 MHz */
    return ext_clk_seen ? 4608000u : (uint32_t)USO_ADC_CLOCK_HZ;
}

ads_result_t ads_read(int32_t *value, uint8_t *status)
{
    uint8_t tx[7] = {RDATA1, 0, 0, 0, 0, 0, 0}, rx[7] = {0};
    select_adc();
    bool ok = transfer(tx, rx, sizeof(tx));
    release_adc();
    if (!ok) return ADS_IO;
    if (rx[1] & 0x80u) ext_clk_seen = true;
    return ads_decode(rx + 1, value, status);
}

ads_result_t ads_read_ex(int32_t *value, uint8_t *status, bool *pga_overflow)
{
    uint8_t tx[7] = {RDATA1, 0, 0, 0, 0, 0, 0}, rx[7] = {0};
    select_adc();
    bool ok = transfer(tx, rx, sizeof(tx));
    release_adc();
    if (!ok) return ADS_IO;
    if (rx[1] & 0x80u) ext_clk_seen = true;
    return ads_decode_ex(rx + 1, value, status, pga_overflow);
}

#endif /* !USO_BOOTLOADER */

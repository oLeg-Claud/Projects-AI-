#include "ads1262.h"
#include "board.h"
#include <string.h>

enum { POWER=1, INTERFACE, MODE0, MODE1, MODE2, INPMUX };
enum { START1=0x08, STOP1=0x0a, RDATA1=0x12, SFOCAL1=0x19 };
static uint8_t offsets[6][3];

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
    tx[0] = 0x20u | addr; tx[1] = n - 1u;
    select_adc();
    bool ok = transfer(tx, rx, n + 2u);
    release_adc();
    memcpy(out, rx + 2, n);
    return ok;
}
static bool write_regs(uint8_t addr, const uint8_t *data, uint8_t n)
{
    uint8_t tx[23] = {0}, rx[23];
    if (!n || n > 21) return false;
    tx[0] = 0x40u | addr; tx[1] = n - 1u;
    memcpy(tx + 2, data, n);
    select_adc();
    bool ok = transfer(tx, rx, n + 2u);
    release_adc();
    return ok;
}
static bool set_reg(uint8_t addr, uint8_t data)
{
    uint8_t actual = 0;
    return write_regs(addr, &data, 1) && read_regs(addr, &actual, 1) && actual == data;
}
bool ads_stop(void) { return command(STOP1); }

bool ads_init(void)
{
    /* START stays low: only START1/STOP1 commands control conversion. */
    board_reset(false); HAL_Delay(10);
    board_reset(true); HAL_Delay(25);
    uint8_t id = 0xff;
    if (!read_regs(0, &id, 1) || (id >> 5) != 0) return false;
    /* Clear RESET flag, keep internal reference; enable VBIAS; CHOP off for nominal 400 SPS. */
    const uint8_t regs[20] = {0x03,0x05,0x00,0x60,0x08,0x89,
                             0,0,0,0,0,0x40,0xbb,0,0,0,0,0,0,0};
    uint8_t verify[20];
    if (!ads_stop() || !write_regs(1, regs, 20) || !read_regs(1, verify, 20) ||
        memcmp(regs, verify, 20)) return false;
    HAL_Delay(50);
    /* Calibrate all six gains once, restore cached offsets on each START. */
    for (uint8_t gain = 0; gain < 6; ++gain) {
        if (!set_reg(MODE2, (gain << 4) | 0x08u) || !set_reg(INPMUX, 0xff) ||
            !command(START1)) return false;
        uint8_t cmd = SFOCAL1, rx;
        select_adc();
        bool ok = transfer(&cmd, &rx, 1);
        /* TI 9.4.9.8: keep CS and SCLK low throughout calibration. */
        HAL_Delay(1);
        uint32_t started = HAL_GetTick();
        while (ok && !board_drdy()) {
            if ((uint32_t)(HAL_GetTick() - started) >= 500u) ok = false;
        }
        release_adc();
        if (!ok || !ads_stop() || !read_regs(0x07, offsets[gain], 3)) return false;
    }
    return set_reg(INPMUX, 0x89);
}
bool ads_start(uint8_t gain)
{
    if (gain > 5) return false;
    uint8_t check[3];
    return ads_stop() && set_reg(MODE2, (gain << 4) | 0x08u) &&
           write_regs(0x07, offsets[gain], 3) && read_regs(0x07, check, 3) &&
           memcmp(check, offsets[gain], 3) == 0 && command(START1);
}
ads_result_t ads_read(int32_t *value, uint8_t *status)
{
    uint8_t tx[7] = {RDATA1,0,0,0,0,0,0}, rx[7] = {0};
    select_adc();
    bool ok = transfer(tx, rx, sizeof(tx));
    release_adc();
    return ok ? ads_decode(rx + 1, value, status) : ADS_IO;
}

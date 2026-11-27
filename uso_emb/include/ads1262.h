#ifndef ADS1262_H
#define ADS1262_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { ADS_OK, ADS_IO, ADS_CHECKSUM, ADS_STALE, ADS_ALARM } ads_result_t;

typedef struct {
    uint8_t gain;       /* 0..5 -> x1, x2, x4, x8, x16, x32 */
    bool    pga_bypass; /* MODE2[7]: 1 = bypass PGA */
    uint8_t dr_code;    /* MODE2[3:0]: 0x04(20)..0x0B(4800) SPS */
    uint8_t filter;     /* MODE1[7:5]: 0=Sinc1..3=Sinc4, 4=FIR */
    bool    chop;       /* MODE0[4]: 1 = CHOP enable */
    bool    vbias;      /* POWER[1]: 1 = level-shift VBIAS enable */
    uint8_t inpmux;     /* INPMUX: 0x89 normal AIN8/9, 0xFF short, 0xBB temp, 0xCC AVDD, 0xDD DVDD */
} ads_config_t;

/* Legacy API (100% preserved) */
bool ads_init(void);
bool ads_start(uint8_t gain);
bool ads_stop(void);
ads_result_t ads_read(int32_t *value, uint8_t *status);
/* Also compiled by host tests. status is not part of the TI checksum. */
ads_result_t ads_decode(const uint8_t bytes[6], int32_t *value, uint8_t *status);

/* Extended V2 API: dynamic rate/filter/CHOP/INPMUX, live gain, recalibration, PGA overload split */
void ads_default_config(ads_config_t *cfg);
bool ads_start_ex(const ads_config_t *cfg);
bool ads_set_gain_live(uint8_t gain, bool pga_bypass);
bool ads_recalibrate(uint8_t dr_code, uint8_t filter, bool chop, int32_t out_offsets[6]);
uint8_t ads_get_chip_id(void);
uint32_t ads_get_clock_hz(void);
ads_result_t ads_read_ex(int32_t *value, uint8_t *status, bool *pga_overflow);
ads_result_t ads_decode_ex(const uint8_t bytes[6], int32_t *value, uint8_t *status, bool *pga_overflow);

#endif

#include "ads1262.h"
#include <limits.h>
#include <string.h>

ads_result_t ads_decode(const uint8_t b[6], int32_t *value, uint8_t *status)
{
    *status = b[0];
    uint8_t sum = (uint8_t)(0x9bu + b[1] + b[2] + b[3] + b[4]);
    if (sum != b[5]) return ADS_CHECKSUM;
    /* Internal ADC clock required; reject RESET and all analog alarms. */
    if ((b[0] & 0x3fu) != 0) return ADS_ALARM;
    if (!(b[0] & 0x40u)) return ADS_STALE;
    uint32_t raw = ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) |
                   ((uint32_t)b[3] << 8) | b[4];
    memcpy(value, &raw, sizeof(raw));
    return ADS_OK;
}

ads_result_t ads_decode_ex(const uint8_t b[6], int32_t *value, uint8_t *status, bool *pga_overflow)
{
    if (status) *status = b[0];
    if (pga_overflow) *pga_overflow = false;

    uint8_t sum = (uint8_t)(0x9bu + b[1] + b[2] + b[3] + b[4]);
    if (sum != b[5]) return ADS_CHECKSUM;

    /* TI SBAS661C Table 9-18: RESET[0], PGAD[1], PGAH[2], PGAL[3],
     * REF_ALM[4], EXTCLK[5]. Clock selection is not an analog alarm. */
    if ((b[0] & 0x11u) != 0u) return ADS_ALARM;
    if (!(b[0] & 0x40u)) return ADS_STALE;

    uint32_t raw = ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) |
                   ((uint32_t)b[3] << 8) | b[4];
    int32_t sample;
    memcpy(&sample, &raw, sizeof(sample));
    if (value) *value = sample;

    if (pga_overflow) {
        if ((b[0] & 0x0Eu) != 0u || sample == INT32_MAX || sample == INT32_MIN) {
            *pga_overflow = true;
        }
    }
    return ADS_OK;
}

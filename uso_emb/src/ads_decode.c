#include "ads1262.h"
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

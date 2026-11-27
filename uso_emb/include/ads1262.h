#ifndef ADS1262_H
#define ADS1262_H
#include <stdbool.h>
#include <stdint.h>
typedef enum { ADS_OK, ADS_IO, ADS_CHECKSUM, ADS_STALE, ADS_ALARM } ads_result_t;
bool ads_init(void);
bool ads_start(uint8_t gain);
bool ads_stop(void);
ads_result_t ads_read(int32_t *value, uint8_t *status);
/* Also compiled by host tests. status is not part of the TI checksum. */
ads_result_t ads_decode(const uint8_t bytes[6], int32_t *value, uint8_t *status);
#endif

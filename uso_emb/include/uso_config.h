#ifndef USO_CONFIG_H
#define USO_CONFIG_H

/* Classic Provodka treats zero as an unidentified device. Override per unit. */
#ifndef USO_DEVICE_NUMBER
#define USO_DEVICE_NUMBER 1u
#endif
#if USO_DEVICE_NUMBER < 1 || USO_DEVICE_NUMBER > 255
#error "USO_DEVICE_NUMBER must be in the range 1..255"
#endif

#define USO_FW_MAJOR 3u
#define USO_FW_MINOR 0u
#define USO_FW_PATCH 0u

/* Internal ADS1262 oscillator = 7.3728 MHz (override to 4608000u for Rev P external X1). */
#ifndef USO_ADC_CLOCK_HZ
#define USO_ADC_CLOCK_HZ 7372800u
#endif

#define USO_SAMPLE_RATE_HZ 400u
#define USO_COMMAND_TIMEOUT_MS 100u
#define USO_V2_TIMEOUT_MS 250u
#define USO_ADC_TIMEOUT_MS 250u
/* No HP transfer function was supplied. Provisional first-order DC blocker. */
#define USO_HP_CUTOFF_HZ 0.1
#define USO_RX_CAPACITY 256u
#define USO_TX_FRAMES 128u
#endif

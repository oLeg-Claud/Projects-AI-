#ifndef BOARD_H
#define BOARD_H
#include "stm32g4xx_hal.h"
#include <stdbool.h>
extern SPI_HandleTypeDef hspi3;
void board_init(void);
void board_delay_us(uint32_t us);
void board_led(bool on);
void board_fatal(void);
void board_adc_irq(bool enable);
static inline void board_cs(bool high) { HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, high ? GPIO_PIN_SET : GPIO_PIN_RESET); }
static inline void board_reset(bool high) { HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, high ? GPIO_PIN_SET : GPIO_PIN_RESET); }
static inline bool board_drdy(void) { return HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_6) == GPIO_PIN_RESET; }
#endif

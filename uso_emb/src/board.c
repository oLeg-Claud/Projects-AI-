#include "board.h"

SPI_HandleTypeDef hspi3;
static void clock_init(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();
    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK) board_fatal();
    RCC_OscInitTypeDef osc = {0};
    /* MCU clock independent of the ADC's internal 7.3728 MHz oscillator. */
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_HSI48;
    osc.HSIState = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    osc.PLL.PLLM = RCC_PLLM_DIV4;
    osc.PLL.PLLN = 72;
    osc.HSI48State = RCC_HSI48_ON;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLP = RCC_PLLP_DIV2;
    osc.PLL.PLLQ = RCC_PLLQ_DIV2;
    osc.PLL.PLLR = RCC_PLLR_DIV2;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) board_fatal();
    RCC_ClkInitTypeDef clk = {0};
    clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_4) != HAL_OK) board_fatal();
    RCC_PeriphCLKInitTypeDef periph = {0};
    periph.PeriphClockSelection = RCC_PERIPHCLK_USB;
    periph.UsbClockSelection = RCC_USBCLKSOURCE_HSI48;
    if (HAL_RCCEx_PeriphCLKConfig(&periph) != HAL_OK) board_fatal();
    __HAL_RCC_CRS_CLK_ENABLE();
    RCC_CRSInitTypeDef crs = {0};
    crs.Prescaler = RCC_CRS_SYNC_DIV1;
    crs.Source = RCC_CRS_SYNC_SOURCE_USB;
    crs.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
    crs.ReloadValue = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000, 1000);
    crs.ErrorLimitValue = RCC_CRS_ERRORLIMIT_DEFAULT;
    crs.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
    HAL_RCCEx_CRSConfig(&crs);
}
void board_init(void)
{
    HAL_Init();
    __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    /* Set safe output levels before changing pin modes. */
    board_cs(true); board_reset(false);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
    GPIO_InitTypeDef g = {0};
    g.Mode = GPIO_MODE_OUTPUT_PP; g.Pull = GPIO_NOPULL; g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Pin = GPIO_PIN_0 | GPIO_PIN_8 | GPIO_PIN_15; HAL_GPIO_Init(GPIOA, &g);
    g.Pin = GPIO_PIN_7; HAL_GPIO_Init(GPIOB, &g);
    clock_init();
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0; DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __HAL_RCC_SPI3_CLK_ENABLE();
    g.Pin = GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5;
    g.Mode = GPIO_MODE_AF_PP; g.Alternate = GPIO_AF6_SPI3; g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &g);
    g.Pin = GPIO_PIN_6; g.Mode = GPIO_MODE_IT_FALLING; g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOB, &g);
    HAL_NVIC_SetPriority(EXTI9_5_IRQn, 2, 0);
    hspi3.Instance = SPI3;
    hspi3.Init.Mode = SPI_MODE_MASTER;
    hspi3.Init.Direction = SPI_DIRECTION_2LINES;
    hspi3.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi3.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi3.Init.CLKPhase = SPI_PHASE_2EDGE;
    hspi3.Init.NSS = SPI_NSS_SOFT;
    hspi3.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_64;
    hspi3.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi3.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi3.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi3.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
    if (HAL_SPI_Init(&hspi3) != HAL_OK) board_fatal();
}
void board_adc_irq(bool enable)
{
    HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
    __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_6);
    HAL_NVIC_ClearPendingIRQ(EXTI9_5_IRQn);
    if (enable) HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
}
void board_delay_us(uint32_t us)
{
    uint32_t begin = DWT->CYCCNT;
    uint32_t cycles = (SystemCoreClock / 1000000u + 1u) * us;
    while ((uint32_t)(DWT->CYCCNT - begin) < cycles) { __NOP(); }
}
void board_led(bool on) { HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, on ? GPIO_PIN_SET : GPIO_PIN_RESET); }
void board_fatal(void)
{
    board_reset(false);
    for (;;) { board_led(true); HAL_Delay(100); board_led(false); HAL_Delay(100); }
}
void SysTick_Handler(void) { HAL_IncTick(); }
void NMI_Handler(void) {
#ifdef USO_BOOTLOADER
    /* Interrupted flash programming may leave an ECC-invalid doubleword.
       Treat it as an invalid image, rather than getting stuck in NMI. */
    if(FLASH->ECCR & FLASH_ECCR_ECCD) {
        extern volatile bool bl_ecc_error;
        bl_ecc_error=true; SET_BIT(FLASH->ECCR,FLASH_ECCR_ECCD); __DSB(); return;
    }
#endif
    HAL_RCC_NMI_IRQHandler();
}
void HAL_RCC_CSSCallback(void)
{
    /* Defensive reset if a clock security interrupt is ever enabled. */
    board_reset(false);
    NVIC_SystemReset();
}
void EXTI9_5_IRQHandler(void) { HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_6); }
void HardFault_Handler(void) { board_reset(false); NVIC_SystemReset(); for (;;) {} }
void MemManage_Handler(void) { HardFault_Handler(); }
void BusFault_Handler(void) { HardFault_Handler(); }
void UsageFault_Handler(void) { HardFault_Handler(); }

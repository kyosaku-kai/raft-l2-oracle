/**
 * System initialization and clock configuration for STM32F207ZG.
 *
 * SystemInit() is called from startup assembly before main.
 * SystemClock_Config() is called from main after HAL_Init().
 *
 * Clock tree: 8MHz HSE (ST-LINK MCO bypass) -> PLL -> 120MHz SYSCLK
 *   AHB  = 120 MHz (div 1)
 *   APB1 = 30 MHz  (div 4, max 30 MHz)
 *   APB2 = 60 MHz  (div 2, max 60 MHz)
 *   USB  = 48 MHz  (PLL_Q = 5)
 *
 * Based on CMSIS system_stm32f2xx.c template from STM32CubeF2 v1.9.6.
 */

#include "stm32f2xx_hal.h"

uint32_t SystemCoreClock = 16000000;
const uint8_t AHBPrescTable[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 6, 7, 8, 9};
const uint8_t APBPrescTable[8]  = {0, 0, 0, 0, 1, 2, 3, 4};

void SystemInit(void)
{
    /* Vector table is at default 0x08000000 (FLASH base), no relocation needed */
}

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    /* HSE from ST-LINK MCO (bypass, not crystal) -> PLL -> 120 MHz */
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState = RCC_HSE_BYPASS;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM = 8;      /* VCO input = 8 MHz / 8 = 1 MHz */
    osc.PLL.PLLN = 240;    /* VCO output = 1 MHz * 240 = 240 MHz */
    osc.PLL.PLLP = RCC_PLLP_DIV2;  /* SYSCLK = 240 / 2 = 120 MHz */
    osc.PLL.PLLQ = 5;      /* USB clock = 240 / 5 = 48 MHz */

    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        /* HSE or PLL failed - stay on HSI, spin for debugger */
        while (1) {}
    }

    /* Configure bus clocks: AHB=120, APB1=30, APB2=60 */
    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                  | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV4;
    clk.APB2CLKDivider = RCC_HCLK_DIV2;

    /* 3 wait states for 120 MHz at 2.7-3.6V (RM0033 Table 10) */
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_3) != HAL_OK) {
        while (1) {}
    }
}

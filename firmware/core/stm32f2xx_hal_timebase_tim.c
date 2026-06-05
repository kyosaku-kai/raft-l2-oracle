/**
 * HAL timebase using TIM6 instead of SysTick.
 *
 * SysTick is reserved exclusively for FreeRTOS. HAL_GetTick() and
 * HAL_Delay() use TIM6 via HAL_InitTick() override (weak in stm32f2xx_hal.c).
 *
 * Based on stm32f2xx_hal_timebase_tim_template.c from STM32CubeF2 v1.9.6.
 * APB1 timer clock = 60 MHz (APB1 prescaler = /4, timers get x2 = 60 MHz).
 */

#include "stm32f2xx_hal.h"

static TIM_HandleTypeDef htim6;

HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
    RCC_ClkInitTypeDef clkconfig;
    uint32_t uwTimclock, uwAPB1Prescaler;
    uint32_t uwPrescalerValue;
    uint32_t pFLatency;
    HAL_StatusTypeDef status;

    __HAL_RCC_TIM6_CLK_ENABLE();

    HAL_RCC_GetClockConfig(&clkconfig, &pFLatency);

    uwAPB1Prescaler = clkconfig.APB1CLKDivider;
    if (uwAPB1Prescaler == RCC_HCLK_DIV1) {
        uwTimclock = HAL_RCC_GetPCLK1Freq();
    } else {
        uwTimclock = 2 * HAL_RCC_GetPCLK1Freq();
    }

    /* Prescale to 1 MHz counter clock */
    uwPrescalerValue = (uwTimclock / 1000000U) - 1U;

    htim6.Instance = TIM6;
    htim6.Init.Period = (1000000U / 1000U) - 1U;  /* 1 ms period */
    htim6.Init.Prescaler = uwPrescalerValue;
    htim6.Init.ClockDivision = 0;
    htim6.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    status = HAL_TIM_Base_Init(&htim6);
    if (status == HAL_OK) {
        status = HAL_TIM_Base_Start_IT(&htim6);
        if (status == HAL_OK) {
            HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
            if (TickPriority < (1UL << __NVIC_PRIO_BITS)) {
                HAL_NVIC_SetPriority(TIM6_DAC_IRQn, TickPriority, 0);
                uwTickPrio = TickPriority;
            } else {
                status = HAL_ERROR;
            }
        }
    }

    return status;
}

void HAL_SuspendTick(void)
{
    __HAL_TIM_DISABLE_IT(&htim6, TIM_IT_UPDATE);
}

void HAL_ResumeTick(void)
{
    __HAL_TIM_ENABLE_IT(&htim6, TIM_IT_UPDATE);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM6) {
        HAL_IncTick();
    }
}

void TIM6_DAC_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim6);
}

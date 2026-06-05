/**
 * Interrupt handlers for STM32F207ZG.
 *
 * SVC_Handler and PendSV_Handler are provided by FreeRTOS port
 * (mapped via #defines in FreeRTOSConfig.h).
 *
 * SysTick is exclusively FreeRTOS's tick source. HAL uses TIM6
 * (see stm32f2xx_hal_timebase_tim.c).
 */

#include "stm32f2xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "../transport/transport_eth.h"

extern void xPortSysTickHandler(void);

void NMI_Handler(void)
{
}

void HardFault_Handler(void)
{
    while (1) {}
}

void MemManage_Handler(void)
{
    while (1) {}
}

void BusFault_Handler(void)
{
    while (1) {}
}

void UsageFault_Handler(void)
{
    while (1) {}
}

void DebugMon_Handler(void)
{
}

void SysTick_Handler(void)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xPortSysTickHandler();
    }
}

void ETH_IRQHandler(void)
{
    ETH_HandleTypeDef *h = eth_transport_get_handle();
    if (h)
        HAL_ETH_IRQHandler(h);
}

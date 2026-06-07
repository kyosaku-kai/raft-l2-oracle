/**
 * Minimal syscall stubs for newlib nano.
 *
 * _write() retargets printf/puts to USART3 (ST-LINK VCP on Nucleo-144).
 * nosys.specs provides the remaining stubs (_read, _close, _lseek, etc.).
 */

#include "stm32f2xx_hal.h"

extern UART_HandleTypeDef huart3;

int _write(int fd, char *ptr, int len)
{
    (void)fd;
    /* Direct register polling - bypasses HAL state machine and HAL_GetTick
     * to avoid issues with HAL timebase (TIM6) interaction with FreeRTOS. */
    USART_TypeDef *uart = huart3.Instance;
    for (int i = 0; i < len; i++) {
        while (!(uart->SR & USART_SR_TXE))
            ;  /* wait for TX empty */
        uart->DR = (uint8_t)ptr[i];
    }
    return len;
}

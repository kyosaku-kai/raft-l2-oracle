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
        /* Timeout: ~10ms at 120MHz. Prevents infinite hang if UART is stuck. */
        volatile uint32_t timeout = 1200000;
        while (!(uart->SR & USART_SR_TXE)) {
            if (--timeout == 0)
                return i;  /* partial write - UART hardware stuck */
        }
        uart->DR = (uint8_t)ptr[i];
    }
    return len;
}

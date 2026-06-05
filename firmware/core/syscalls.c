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
    HAL_UART_Transmit(&huart3, (uint8_t *)ptr, (uint16_t)len, HAL_MAX_DELAY);
    return len;
}

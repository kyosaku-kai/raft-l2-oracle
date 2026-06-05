/**
 * STM32F2xx HAL configuration for raft-l2-oracle firmware.
 * Based on stm32f2xx_hal_conf_template.h from STM32CubeF2 v1.9.6.
 * Only modules needed for this project are enabled.
 */

#ifndef __STM32F2xx_HAL_CONF_H
#define __STM32F2xx_HAL_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/* Module enables - only what we need */
#define HAL_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED
#define HAL_ETH_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_TIM_MODULE_ENABLED

/* Oscillator values for Nucleo-F207ZG */
#if !defined(HSE_VALUE)
#define HSE_VALUE 8000000U
#endif

#if !defined(HSE_STARTUP_TIMEOUT)
#define HSE_STARTUP_TIMEOUT 100U
#endif

#if !defined(HSI_VALUE)
#define HSI_VALUE 16000000U
#endif

#if !defined(LSI_VALUE)
#define LSI_VALUE 32000U
#endif

#if !defined(LSE_VALUE)
#define LSE_VALUE 32768U
#endif

#if !defined(LSE_STARTUP_TIMEOUT)
#define LSE_STARTUP_TIMEOUT 5000U
#endif

#if !defined(EXTERNAL_CLOCK_VALUE)
#define EXTERNAL_CLOCK_VALUE 12288000U
#endif

/* System configuration */
#define VDD_VALUE                 3300U
#define TICK_INT_PRIORITY         0x0FU
#define USE_RTOS                  0U
#define PREFETCH_ENABLE           1U
#define INSTRUCTION_CACHE_ENABLE  1U
#define DATA_CACHE_ENABLE         1U

/* Ethernet configuration */
#define MAC_ADDR0 2U
#define MAC_ADDR1 0U
#define MAC_ADDR2 0U
#define MAC_ADDR3 0U
#define MAC_ADDR4 0U
#define MAC_ADDR5 0U

#define ETH_RX_BUF_SIZE ETH_MAX_PACKET_SIZE
#define ETH_TX_BUF_SIZE ETH_MAX_PACKET_SIZE
#define ETH_RXBUFNB      4U
#define ETH_TXBUFNB      4U

/* LAN8742A PHY on Nucleo-F207ZG (address 0) */
#define LAN8742A_PHY_ADDRESS 0x00U

#define PHY_RESET_DELAY       0x000000FFU
#define PHY_CONFIG_DELAY      0x00000FFFU
#define PHY_READ_TO           0x0000FFFFU
#define PHY_WRITE_TO          0x0000FFFFU

#define PHY_BCR                 ((uint16_t)0x0000)
#define PHY_BSR                 ((uint16_t)0x0001)

#define PHY_RESET               ((uint16_t)0x8000)
#define PHY_LOOPBACK            ((uint16_t)0x4000)
#define PHY_FULLDUPLEX_100M     ((uint16_t)0x2100)
#define PHY_HALFDUPLEX_100M     ((uint16_t)0x2000)
#define PHY_FULLDUPLEX_10M      ((uint16_t)0x0100)
#define PHY_HALFDUPLEX_10M      ((uint16_t)0x0000)
#define PHY_AUTONEGOTIATION     ((uint16_t)0x1000)
#define PHY_RESTART_AUTONEGOTIATION ((uint16_t)0x0200)
#define PHY_POWERDOWN           ((uint16_t)0x0800)
#define PHY_ISOLATE             ((uint16_t)0x0400)

#define PHY_AUTONEGO_COMPLETE   ((uint16_t)0x0020)
#define PHY_LINKED_STATUS       ((uint16_t)0x0004)
#define PHY_JABBER_DETECTION    ((uint16_t)0x0002)

/* LAN8742A special status register */
#define PHY_SR                  ((uint16_t)0x001F)
#define PHY_SPEED_STATUS        ((uint16_t)0x0004)
#define PHY_DUPLEX_STATUS       ((uint16_t)0x0010)

#define PHY_ISFR                ((uint16_t)0x001D)
#define PHY_ISFR_INT4           ((uint16_t)0x000B)

/* Include enabled module headers */
#ifdef HAL_RCC_MODULE_ENABLED
#include "stm32f2xx_hal_rcc.h"
#endif

#ifdef HAL_GPIO_MODULE_ENABLED
#include "stm32f2xx_hal_gpio.h"
#endif

#ifdef HAL_DMA_MODULE_ENABLED
#include "stm32f2xx_hal_dma.h"
#endif

#ifdef HAL_CORTEX_MODULE_ENABLED
#include "stm32f2xx_hal_cortex.h"
#endif

#ifdef HAL_ETH_MODULE_ENABLED
#include "stm32f2xx_hal_eth.h"
#endif

#ifdef HAL_FLASH_MODULE_ENABLED
#include "stm32f2xx_hal_flash.h"
#endif

#ifdef HAL_PWR_MODULE_ENABLED
#include "stm32f2xx_hal_pwr.h"
#endif

#ifdef HAL_TIM_MODULE_ENABLED
#include "stm32f2xx_hal_tim.h"
#endif

#ifdef HAL_UART_MODULE_ENABLED
#include "stm32f2xx_hal_uart.h"
#endif

/* Assert configuration */
#ifdef USE_FULL_ASSERT
#define assert_param(expr) ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))
void assert_failed(uint8_t *file, uint32_t line);
#else
#define assert_param(expr) ((void)0U)
#endif

#ifdef __cplusplus
}
#endif

#endif /* __STM32F2xx_HAL_CONF_H */

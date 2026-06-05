#ifndef TRANSPORT_ETH_H
#define TRANSPORT_ETH_H

/**
 * transport_eth.h - STM32 Ethernet transport for L2 Oracle
 *
 * Implements raft_transport_t over the STM32F207 Ethernet MAC + LAN8742A PHY.
 * Raw Ethernet II frames with custom EtherTypes (0x88B5/0x88B6/0x88B7).
 * Uses HAL ETH driver with DMA, interrupt-driven RX, task-context processing.
 */

#include "transport.h"
#include "stm32f2xx_hal.h"

/**
 * Create an STM32 Ethernet transport instance.
 *
 * Initializes ETH MAC, LAN8742A PHY (RMII), DMA descriptors, and starts
 * the MAC. Must be called after HAL_Init() and SystemClock_Config().
 * Blocks during PHY autonegotiation (~3-5 seconds).
 *
 * @param node_id  This node's ID (1-based)
 * @param box_id   This node's box ID (used in MAC: 02:CA:FE:<box>:00:01)
 * @return Transport instance, or NULL on init failure
 */
raft_transport_t *eth_transport_create(uint8_t node_id, uint8_t box_id);

/**
 * Destroy ETH transport. Stops MAC and DMA.
 */
void eth_transport_destroy(raft_transport_t *t);

/**
 * Get the ETH handle for use by ETH_IRQHandler in stm32f2xx_it.c.
 * Returns NULL if transport not yet initialized.
 */
ETH_HandleTypeDef *eth_transport_get_handle(void);

/** Transport statistics for debug/monitoring. */
typedef struct {
    uint32_t tx_frames;
    uint32_t tx_errors;
    uint32_t rx_frames;
    uint32_t rx_dropped;
    uint32_t rx_filtered;
    uint32_t dma_errors;
} eth_transport_stats_t;

void eth_transport_get_stats(raft_transport_t *t, eth_transport_stats_t *out);

#endif /* TRANSPORT_ETH_H */

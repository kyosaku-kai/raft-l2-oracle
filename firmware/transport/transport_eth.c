/**
 * transport_eth.c - STM32 Ethernet transport for L2 Oracle
 *
 * Implements raft_transport_t over STM32F207 Ethernet MAC + LAN8742A PHY.
 * Uses HAL ETH driver with DMA for raw Ethernet II frame TX/RX.
 *
 * RX path: HAL_ETH_RxCpltCallback (ISR) signals a binary semaphore.
 *          recv() takes the semaphore, calls HAL_ETH_GetReceivedFrame_IT
 *          from task context (avoids HAL_LOCK issues in ISR).
 *
 * TX path: send() builds oracle frame in DMA TX buffer, calls
 *          HAL_ETH_TransmitFrame. Mutex-protected for thread safety.
 */

#include "transport_eth.h"
#include "../protocol/wire_format.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <string.h>

/* --- Static DMA resources (not in FreeRTOS heap) --- */

static ETH_DMADescTypeDef rx_dma_desc[ETH_RXBUFNB] __attribute__((aligned(4)));
static ETH_DMADescTypeDef tx_dma_desc[ETH_TXBUFNB] __attribute__((aligned(4)));

static uint8_t rx_buf[ETH_RXBUFNB][ETH_RX_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t tx_buf[ETH_TXBUFNB][ETH_TX_BUF_SIZE] __attribute__((aligned(4)));

/* --- Singleton transport state --- */

typedef struct {
    ETH_HandleTypeDef   heth;
    SemaphoreHandle_t   rx_sem;
    SemaphoreHandle_t   tx_mutex;
    uint8_t             node_id;
    uint8_t             box_id;
    uint8_t             src_mac[6];
    uint8_t             initialized;
    eth_transport_stats_t stats;
} eth_transport_data_t;

static eth_transport_data_t eth_data;
static raft_transport_t     eth_transport_inst;

/* Broadcast MAC for TX (all sends are broadcast in T6) */
static const uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* Minimum Ethernet frame data size (excluding 4-byte FCS added by MAC) */
#define ETH_MIN_FRAME_DATA 60

/* --- HAL_ETH_MspInit: GPIO + clocks + NVIC for RMII --- */

void HAL_ETH_MspInit(ETH_HandleTypeDef *heth)
{
    GPIO_InitTypeDef gpio = {0};

    (void)heth;

    /* Enable GPIO clocks */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    /* Enable Ethernet clocks */
    __HAL_RCC_ETHMAC_CLK_ENABLE();
    __HAL_RCC_ETHMACTX_CLK_ENABLE();
    __HAL_RCC_ETHMACRX_CLK_ENABLE();

    /*
     * RMII pin configuration (all AF11, high speed, no pull):
     *   PA1  - ETH_RMII_REF_CLK
     *   PA2  - ETH_RMII_MDIO
     *   PA7  - ETH_RMII_CRS_DV
     *   PC1  - ETH_RMII_MDC
     *   PC4  - ETH_RMII_RXD0
     *   PC5  - ETH_RMII_RXD1
     *   PG11 - ETH_RMII_TX_EN
     *   PG13 - ETH_RMII_TXD0
     *   PB13 - ETH_RMII_TXD1
     */

    /* PA1, PA2, PA7 */
    gpio.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF11_ETH;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* PB13 */
    gpio.Pin = GPIO_PIN_13;
    HAL_GPIO_Init(GPIOB, &gpio);

    /* PC1, PC4, PC5 */
    gpio.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOC, &gpio);

    /* PG11, PG13 */
    gpio.Pin = GPIO_PIN_11 | GPIO_PIN_13;
    HAL_GPIO_Init(GPIOG, &gpio);

    /* ETH interrupt: priority 5 = configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY
     * (highest priority that can safely call FreeRTOS ...FromISR APIs) */
    HAL_NVIC_SetPriority(ETH_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(ETH_IRQn);
}

void HAL_ETH_MspDeInit(ETH_HandleTypeDef *heth)
{
    (void)heth;

    HAL_NVIC_DisableIRQ(ETH_IRQn);

    __HAL_RCC_ETHMAC_CLK_DISABLE();
    __HAL_RCC_ETHMACTX_CLK_DISABLE();
    __HAL_RCC_ETHMACRX_CLK_DISABLE();
}

/* --- HAL RX complete callback (ISR context) --- */

void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *heth)
{
    (void)heth;
    BaseType_t woken = pdFALSE;

    /* Signal task that frame(s) are available. No frame processing here. */
    xSemaphoreGiveFromISR(eth_data.rx_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

/* --- HAL error callback (ISR context) --- */

void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *heth)
{
    (void)heth;
    eth_data.stats.dma_errors++;
}

/* --- Transport interface: now_ms --- */

static uint64_t eth_now_ms(raft_transport_t *t)
{
    (void)t;
    return (uint64_t)xTaskGetTickCount();
}

/* --- Transport interface: send --- */

static int eth_send(raft_transport_t *t, uint8_t dst_node, uint16_t ethertype,
                    raft_msg_type_t type, const void *payload, size_t len)
{
    (void)dst_node; /* broadcast for T6, unicast table added in T8 */
    eth_transport_data_t *ed = (eth_transport_data_t *)t->impl_data;

    if (len > ORACLE_MAX_PAYLOAD_V1)
        return -1;

    if (xSemaphoreTake(ed->tx_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ed->stats.tx_errors++;
        return -1;
    }

    /* Check descriptor is available (OWN == 0 means CPU owns it) */
    if ((ed->heth.TxDesc->Status & ETH_DMATXDESC_OWN) != 0) {
        xSemaphoreGive(ed->tx_mutex);
        ed->stats.tx_errors++;
        return -1;
    }

    /* Build frame directly in the DMA TX buffer */
    uint8_t *buf = (uint8_t *)ed->heth.TxDesc->Buffer1Addr;
    oracle_frame_header_t *hdr = (oracle_frame_header_t *)buf;

    oracle_frame_init(hdr, broadcast_mac, ed->src_mac, ethertype,
                      (uint8_t)type, ed->node_id, ed->box_id, 0, (uint16_t)len);

    if (len > 0)
        memcpy(buf + sizeof(oracle_frame_header_t), payload, len);

    uint32_t frame_len = (uint32_t)(sizeof(oracle_frame_header_t) + len);

    /* Pad to minimum Ethernet frame size (60 bytes data, MAC adds 4-byte FCS) */
    if (frame_len < ETH_MIN_FRAME_DATA) {
        memset(buf + frame_len, 0, ETH_MIN_FRAME_DATA - frame_len);
        frame_len = ETH_MIN_FRAME_DATA;
    }

    /* Memory barrier before triggering DMA */
    __DMB();

    HAL_StatusTypeDef rc = HAL_ETH_TransmitFrame(&ed->heth, frame_len);

    xSemaphoreGive(ed->tx_mutex);

    if (rc != HAL_OK) {
        ed->stats.tx_errors++;
        return -1;
    }

    ed->stats.tx_frames++;
    return 0;
}

/* --- RX descriptor release helper --- */

static void rx_descriptors_release(ETH_HandleTypeDef *heth)
{
    /* Give all consumed RX descriptors back to DMA */
    ETH_DMADescTypeDef *desc = heth->RxFrameInfos.FSRxDesc;
    uint32_t i;

    for (i = 0; i < heth->RxFrameInfos.SegCount; i++) {
        desc->Status |= ETH_DMARXDESC_OWN;
        desc = (ETH_DMADescTypeDef *)desc->Buffer2NextDescAddr;
    }

    /* Clear segment count for next frame */
    heth->RxFrameInfos.SegCount = 0;

    /* Memory barrier before resuming DMA */
    __DMB();

    /* Resume DMA reception if suspended (buffer unavailable) */
    if ((heth->Instance->DMASR & ETH_DMASR_RBUS) != 0) {
        heth->Instance->DMASR = ETH_DMASR_RBUS;
        heth->Instance->DMARPDR = 0;
    }
}

/* --- Transport interface: recv --- */

static int eth_recv(raft_transport_t *t, uint16_t *ethertype,
                    raft_msg_type_t *type, uint8_t *src_node,
                    void *payload, size_t max_len, uint32_t timeout_ms)
{
    eth_transport_data_t *ed = (eth_transport_data_t *)t->impl_data;

    /* Block until ISR signals a frame is available, or timeout */
    if (xSemaphoreTake(ed->rx_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return 0; /* timeout */

    /* Process received frame(s) from task context (safe for HAL_LOCK) */
    if (HAL_ETH_GetReceivedFrame_IT(&ed->heth) != HAL_OK) {
        /* Spurious wakeup or descriptor not ready */
        return 0;
    }

    uint8_t *frame = (uint8_t *)ed->heth.RxFrameInfos.buffer;
    uint32_t frame_len = ed->heth.RxFrameInfos.length;

    /* Minimum: 14-byte Ethernet header */
    if (frame_len < 14) {
        ed->stats.rx_dropped++;
        rx_descriptors_release(&ed->heth);
        return -1;
    }

    /* Extract EtherType (bytes 12-13, big-endian on wire) */
    uint16_t etype = ((uint16_t)frame[12] << 8) | frame[13];

    /* Filter: only accept our oracle EtherTypes */
    if (etype != ETHERTYPE_RAFT && etype != ETHERTYPE_HEALTH && etype != ETHERTYPE_HEARTBEAT) {
        ed->stats.rx_filtered++;
        rx_descriptors_release(&ed->heth);
        return -1;
    }

    /* Need at least a full oracle header (24 bytes) */
    if (frame_len < sizeof(oracle_frame_header_t)) {
        ed->stats.rx_dropped++;
        rx_descriptors_release(&ed->heth);
        return -1;
    }

    /* Parse oracle header */
    oracle_frame_header_t *hdr = (oracle_frame_header_t *)frame;

    /* Version check (upper nibble) */
    if ((hdr->version & 0xF0) != (ORACLE_PROTOCOL_VERSION & 0xF0)) {
        ed->stats.rx_dropped++;
        rx_descriptors_release(&ed->heth);
        return -1;
    }

    *ethertype = etype;
    *type = (raft_msg_type_t)hdr->msg_type;
    *src_node = hdr->node_id;

    /* Copy payload to caller buffer */
    uint16_t payload_len = hdr->payload_len;
    if (payload_len > max_len)
        payload_len = (uint16_t)max_len;
    if (payload_len > ORACLE_MAX_PAYLOAD_V1)
        payload_len = ORACLE_MAX_PAYLOAD_V1;

    if (payload_len > 0)
        memcpy(payload, frame + sizeof(oracle_frame_header_t), payload_len);

    ed->stats.rx_frames++;
    rx_descriptors_release(&ed->heth);

    return (int)payload_len;
}

/* --- Public API --- */

raft_transport_t *eth_transport_create(uint8_t node_id, uint8_t box_id)
{
    memset(&eth_data, 0, sizeof(eth_data));

    eth_data.node_id = node_id;
    eth_data.box_id = box_id;
    ORACLE_MAC_STM32(eth_data.src_mac, box_id);

    /* Configure ETH handle */
    eth_data.heth.Instance = ETH;
    eth_data.heth.Init.AutoNegotiation = ETH_AUTONEGOTIATION_ENABLE;
    eth_data.heth.Init.Speed = ETH_SPEED_100M;
    eth_data.heth.Init.DuplexMode = ETH_MODE_FULLDUPLEX;
    eth_data.heth.Init.PhyAddress = LAN8742A_PHY_ADDRESS;
    eth_data.heth.Init.MACAddr = eth_data.src_mac;
    eth_data.heth.Init.RxMode = ETH_RXINTERRUPT_MODE;
    eth_data.heth.Init.ChecksumMode = ETH_CHECKSUM_BY_SOFTWARE;
    eth_data.heth.Init.MediaInterface = ETH_MEDIA_INTERFACE_RMII;

    /* HAL_ETH_Init calls HAL_ETH_MspInit (GPIO, clocks, NVIC).
     * Blocks during PHY autonegotiation (~3-5 seconds if cable connected,
     * timeout if no cable). */
    if (HAL_ETH_Init(&eth_data.heth) != HAL_OK)
        return NULL;

    /* Initialize DMA descriptor lists */
    HAL_ETH_DMATxDescListInit(&eth_data.heth, tx_dma_desc,
                              &tx_buf[0][0], ETH_TXBUFNB);
    HAL_ETH_DMARxDescListInit(&eth_data.heth, rx_dma_desc,
                              &rx_buf[0][0], ETH_RXBUFNB);

    /* Enable RX DMA complete interrupt on each descriptor */
    for (uint32_t i = 0; i < ETH_RXBUFNB; i++) {
        /* DIC = Disable Interrupt on Completion. Clear it to ENABLE interrupt. */
        rx_dma_desc[i].ControlBufferSize &= ~ETH_DMARXDESC_DIC;
    }

    /* Enable promiscuous mode: accept all frames regardless of MAC address.
     * We filter by EtherType in software. */
    eth_data.heth.Instance->MACFFR |= ETH_MACFFR_PM;

    /* Create FreeRTOS synchronization primitives */
    eth_data.rx_sem = xSemaphoreCreateBinary();
    eth_data.tx_mutex = xSemaphoreCreateMutex();

    if (!eth_data.rx_sem || !eth_data.tx_mutex)
        return NULL;

    /* Start MAC and DMA */
    HAL_ETH_Start(&eth_data.heth);

    /* Wire transport vtable */
    eth_transport_inst.send = eth_send;
    eth_transport_inst.recv = eth_recv;
    eth_transport_inst.now_ms = eth_now_ms;
    eth_transport_inst.impl_data = &eth_data;

    eth_data.initialized = 1;

    return &eth_transport_inst;
}

void eth_transport_destroy(raft_transport_t *t)
{
    if (!t) return;
    eth_transport_data_t *ed = (eth_transport_data_t *)t->impl_data;

    HAL_ETH_Stop(&ed->heth);
    HAL_ETH_DeInit(&ed->heth);

    if (ed->rx_sem) vSemaphoreDelete(ed->rx_sem);
    if (ed->tx_mutex) vSemaphoreDelete(ed->tx_mutex);

    ed->initialized = 0;
}

ETH_HandleTypeDef *eth_transport_get_handle(void)
{
    return eth_data.initialized ? &eth_data.heth : NULL;
}

void eth_transport_get_stats(raft_transport_t *t, eth_transport_stats_t *out)
{
    eth_transport_data_t *ed = (eth_transport_data_t *)t->impl_data;
    *out = ed->stats;
}

/**
 * STM32F207ZG firmware entry point - FreeRTOS task architecture (T8).
 *
 * Task layout (design doc Section 6):
 *   eth_rx_task  (pri 4, 2KB) - demux RX frames by EtherType into queues
 *   raft_task    (pri 3, 4KB) - drain raft_inbox, call oracle_dispatch + periodic
 *   health_task  (pri 2, 2KB) - drain hb_inbox (stub - real logic in T9)
 *   monitor_task (pri 1, 1KB) - periodic stats (HWM, queue depths, heap)
 *
 * Queues:
 *   raft_inbox  - 0x88B5 frames from eth_rx_task -> raft_task
 *   hb_inbox    - 0x88B7 frames from eth_rx_task -> health_task
 *
 * Timer:
 *   raft_timer  - fires every ORACLE_TICK_MS, notifies raft_task
 */

#include "stm32f2xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#include <stdio.h>
#include <string.h>

#include "transport_eth.h"
#include "wire_format.h"
#include "raft_heap.h"
#include "raft_oracle.h"
#include "health_monitor.h"

UART_HandleTypeDef huart3;

/* --- Shared state --- */

static raft_transport_t *g_transport;
static oracle_node_ctx_t g_oracle_ctx;
static health_monitor_t g_health_mon;

/* Task handles (needed for task notifications and HWM queries) */
static TaskHandle_t h_eth_rx;
static TaskHandle_t h_raft;
static TaskHandle_t h_health;
static TaskHandle_t h_monitor;

/* --- Queue message type --- */

typedef struct {
    raft_msg_type_t msg_type;
    uint8_t         src_node;
    uint16_t        payload_len;
    uint8_t         payload[ORACLE_MAX_PAYLOAD_V1];
} rx_msg_t;

/* Queues: sized for burst tolerance without excessive RAM */
#define RAFT_INBOX_DEPTH  16
#define HB_INBOX_DEPTH    8

static QueueHandle_t raft_inbox;
static QueueHandle_t hb_inbox;

/* Software timer handle */
static TimerHandle_t raft_timer;

/* LED pin definitions - Nucleo-F207ZG LD1/LD2/LD3 on PB0/PB7/PB14 */
#define LED_GREEN_PIN   GPIO_PIN_0
#define LED_BLUE_PIN    GPIO_PIN_7
#define LED_RED_PIN     GPIO_PIN_14
#define LED_PORT        GPIOB

/* --- Hardware init (unchanged from T5) --- */

static void GPIO_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();

    gpio.Pin = LED_GREEN_PIN | LED_BLUE_PIN | LED_RED_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_PORT, &gpio);
}

static void USART3_Init(void)
{
    huart3.Instance = USART3;
    huart3.Init.BaudRate = 115200;
    huart3.Init.WordLength = UART_WORDLENGTH_8B;
    huart3.Init.StopBits = UART_STOPBITS_1;
    huart3.Init.Parity = UART_PARITY_NONE;
    huart3.Init.Mode = UART_MODE_TX_RX;
    huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart3.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart3);
}

void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        GPIO_InitTypeDef gpio = {0};

        __HAL_RCC_USART3_CLK_ENABLE();
        __HAL_RCC_GPIOD_CLK_ENABLE();

        gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9;
        gpio.Mode = GPIO_MODE_AF_PP;
        gpio.Pull = GPIO_PULLUP;
        gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        gpio.Alternate = GPIO_AF7_USART3;
        HAL_GPIO_Init(GPIOD, &gpio);
    }
}

extern void SystemClock_Config(void);

/* --- eth_rx_task: demux incoming frames by EtherType --- */

static void vEthRxTask(void *pvParameters)
{
    (void)pvParameters;
    raft_transport_t *tp = g_transport;

    printf("eth_rx: started\r\n");

    for (;;) {
        uint16_t ethertype;
        raft_msg_type_t msg_type;
        uint8_t src_node;
        uint8_t payload[ORACLE_MAX_PAYLOAD_V1];

        int len = tp->recv(tp, &ethertype, &msg_type, &src_node,
                           payload, sizeof(payload), portMAX_DELAY);
        if (len <= 0)
            continue;

        rx_msg_t msg;
        msg.msg_type = msg_type;
        msg.src_node = src_node;
        msg.payload_len = (uint16_t)len;
        memcpy(msg.payload, payload, (size_t)len);

        if (ethertype == ETHERTYPE_RAFT) {
            if (xQueueSend(raft_inbox, &msg, 0) != pdTRUE) {
                /* Queue full - drop oldest to make room */
                rx_msg_t discard;
                xQueueReceive(raft_inbox, &discard, 0);
                xQueueSend(raft_inbox, &msg, 0);
            }
        } else if (ethertype == ETHERTYPE_HEARTBEAT) {
            if (xQueueSend(hb_inbox, &msg, 0) != pdTRUE) {
                rx_msg_t discard;
                xQueueReceive(hb_inbox, &discard, 0);
                xQueueSend(hb_inbox, &msg, 0);
            }
        }
        /* 0x88B6 (health reports) are outbound-only, ignore if received */
    }
}

/* --- raft_timer callback: notify raft_task to call raft_periodic --- */

static void vRaftTimerCallback(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (h_raft)
        xTaskNotifyGive(h_raft);
}

/* --- raft_task: process Raft messages + periodic tick --- */

static void vRaftTask(void *pvParameters)
{
    (void)pvParameters;
    oracle_node_ctx_t *ctx = &g_oracle_ctx;
    TickType_t last_tick = xTaskGetTickCount();

    printf("raft: task started\r\n");

    for (;;) {
        /* Wait for either a queued message or timer notification.
         * Use a short timeout so we can drain the queue responsively. */
        rx_msg_t msg;
        if (xQueueReceive(raft_inbox, &msg, pdMS_TO_TICKS(ORACLE_TICK_MS)) == pdTRUE) {
            oracle_dispatch_raft_message(ctx, msg.src_node, msg.msg_type,
                                         msg.payload, msg.payload_len);
        }

        /* Drain any remaining queued messages (non-blocking) */
        while (xQueueReceive(raft_inbox, &msg, 0) == pdTRUE) {
            oracle_dispatch_raft_message(ctx, msg.src_node, msg.msg_type,
                                         msg.payload, msg.payload_len);
        }

        /* Check if timer fired (consume all pending notifications) */
        if (ulTaskNotifyTake(pdTRUE, 0) > 0) {
            TickType_t now = xTaskGetTickCount();
            int elapsed = (int)(now - last_tick);
            last_tick = now;
            oracle_tick(ctx, elapsed);

            /* LED: green = leader */
            HAL_GPIO_WritePin(LED_PORT, LED_GREEN_PIN,
                              oracle_is_leader(ctx) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        }
    }
}

/* --- health_task: monitor compute node heartbeats via state machine --- */

static void vHealthTask(void *pvParameters)
{
    (void)pvParameters;
    uint32_t hb_count = 0;
    uint32_t last_report = 0;

    printf("health: task started\r\n");

    for (;;) {
        rx_msg_t msg;
        TickType_t now = xTaskGetTickCount();

        /* 10ms timeout matches heartbeat interval for responsive miss detection */
        if (xQueueReceive(hb_inbox, &msg, pdMS_TO_TICKS(HB_INTERVAL_MS)) == pdTRUE) {
            now = xTaskGetTickCount();

            if (msg.msg_type == MSG_NODE_HEARTBEAT &&
                msg.payload_len >= sizeof(payload_node_heartbeat_t))
            {
                hb_count++;
                HAL_GPIO_TogglePin(LED_PORT, LED_BLUE_PIN);
                health_monitor_process_heartbeat(
                    &g_health_mon, msg.src_node,
                    (const payload_node_heartbeat_t *)msg.payload,
                    (uint32_t)now);
            }
            else if (msg.msg_type == MSG_NODE_ANNOUNCE &&
                     msg.payload_len >= sizeof(payload_node_announce_t))
            {
                health_monitor_process_announce(
                    &g_health_mon, msg.src_node,
                    (const payload_node_announce_t *)msg.payload,
                    (uint32_t)now);
            }
        }

        /* Drive state machine on every iteration */
        health_monitor_tick(&g_health_mon, (uint32_t)now);

        /* Periodic stats */
        if (now - last_report >= pdMS_TO_TICKS(5000)) {
            last_report = now;
            uint8_t n_nodes;
            const node_health_entry_t *tbl =
                health_monitor_get_table(&g_health_mon, &n_nodes);
            printf("health: %lu hb, %u nodes tracked\r\n",
                   (unsigned long)hb_count, n_nodes);
            for (uint8_t i = 0; i < n_nodes; i++) {
                const char *st = "?";
                switch (tbl[i].status) {
                case NODE_UP:      st = "UP";      break;
                case NODE_SUSPECT: st = "SUSPECT"; break;
                case NODE_DOWN:    st = "DOWN";    break;
                }
                printf("  node %d: %s (last=%lu miss=%u)\r\n",
                       tbl[i].node_id, st,
                       (unsigned long)tbl[i].last_seen_ms,
                       g_health_mon.miss_count[i]);
            }
        }
    }
}

/* --- monitor_task: stats reporting --- */

static void vMonitorTask(void *pvParameters)
{
    (void)pvParameters;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(5000));

        /* Heap stats */
        size_t free_heap = xPortGetFreeHeapSize();
        size_t min_ever = xPortGetMinimumEverFreeHeapSize();
        printf("[%lu ms] heap: %u free, %u min-ever\r\n",
               (unsigned long)xTaskGetTickCount(),
               (unsigned)free_heap, (unsigned)min_ever);

        /* Stack high water marks (in words, multiply by 4 for bytes) */
        printf("  hwm: rx=%u raft=%u health=%u mon=%u\r\n",
               (unsigned)uxTaskGetStackHighWaterMark(h_eth_rx),
               (unsigned)uxTaskGetStackHighWaterMark(h_raft),
               (unsigned)uxTaskGetStackHighWaterMark(h_health),
               (unsigned)uxTaskGetStackHighWaterMark(h_monitor));

        /* Queue depths */
        printf("  queues: raft=%u/%d hb=%u/%d\r\n",
               (unsigned)uxQueueMessagesWaiting(raft_inbox), RAFT_INBOX_DEPTH,
               (unsigned)uxQueueMessagesWaiting(hb_inbox), HB_INBOX_DEPTH);

        /* Raft state */
        if (g_oracle_ctx.raft) {
            const char *state = "???";
            if (oracle_is_leader(&g_oracle_ctx))
                state = "LEADER";
            else if (raft_is_candidate(g_oracle_ctx.raft))
                state = "CANDIDATE";
            else if (raft_is_follower(g_oracle_ctx.raft))
                state = "FOLLOWER";

            printf("  raft: state=%s term=%ld leader=%d\r\n",
                   state,
                   raft_get_current_term(g_oracle_ctx.raft),
                   (int)raft_get_current_leader(g_oracle_ctx.raft));
        }

        /* Transport stats */
        if (g_transport) {
            eth_transport_stats_t stats;
            eth_transport_get_stats(g_transport, &stats);
            printf("  eth: tx=%lu err=%lu rx=%lu drop=%lu filt=%lu dma=%lu\r\n",
                   (unsigned long)stats.tx_frames,
                   (unsigned long)stats.tx_errors,
                   (unsigned long)stats.rx_frames,
                   (unsigned long)stats.rx_dropped,
                   (unsigned long)stats.rx_filtered,
                   (unsigned long)stats.dma_errors);
        }
    }
}

/* --- FreeRTOS hooks --- */

void assert_failed(const char *file, int line)
{
    printf("ASSERT: %s:%d\r\n", file, line);
    __disable_irq();
    while (1) {}
}

void vApplicationMallocFailedHook(void)
{
    printf("FATAL: pvPortMalloc failed\r\n");
    HAL_GPIO_WritePin(LED_PORT, LED_RED_PIN, GPIO_PIN_SET);
    while (1) {}
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("FATAL: stack overflow in %s\r\n", pcTaskName);
    HAL_GPIO_WritePin(LED_PORT, LED_RED_PIN, GPIO_PIN_SET);
    while (1) {}
}

/* --- main --- */

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    GPIO_Init();
    USART3_Init();

    printf("boot ok (T8 task arch)\r\n");
    printf("heap total: %u bytes\r\n", (unsigned)configTOTAL_HEAP_SIZE);

    /* Initialize Ethernet transport (blocks ~3-5s for PHY autoneg) */
    printf("ETH: initializing...\r\n");
    g_transport = eth_transport_create(1, 1);
    if (g_transport) {
        printf("ETH: init ok, MAC=02:CA:FE:01:00:01\r\n");
    } else {
        printf("ETH: init FAILED (no cable?)\r\n");
    }

    /* Install bare-metal allocators before any raft calls */
    raft_heap_init();

    /* Initialize raft oracle */
    size_t heap_before = xPortGetFreeHeapSize();

    int oracle_rc = oracle_init(&g_oracle_ctx, 1, 1, g_transport);
    if (oracle_rc == 0) {
        oracle_add_node(&g_oracle_ctx, 1, 1); /* self */
        size_t heap_after = xPortGetFreeHeapSize();
        printf("raft: init ok, heap delta=%u bytes\r\n",
               (unsigned)(heap_before - heap_after));
        printf("raft: heap free=%u min-ever=%u\r\n",
               (unsigned)heap_after,
               (unsigned)xPortGetMinimumEverFreeHeapSize());
    } else {
        printf("raft: init FAILED\r\n");
    }

    /* Initialize health monitor and link to oracle context */
    health_monitor_init(&g_health_mon,
                        (struct oracle_node_ctx *)&g_oracle_ctx,
                        g_transport, 1);
    g_oracle_ctx.health_ctx = &g_health_mon;

    /* Create queues */
    raft_inbox = xQueueCreate(RAFT_INBOX_DEPTH, sizeof(rx_msg_t));
    hb_inbox   = xQueueCreate(HB_INBOX_DEPTH, sizeof(rx_msg_t));

    if (!raft_inbox || !hb_inbox) {
        printf("FATAL: queue creation failed\r\n");
        HAL_GPIO_WritePin(LED_PORT, LED_RED_PIN, GPIO_PIN_SET);
        while (1) {}
    }

    /* Create tasks - stack sizes in words (x4 for bytes) */
    xTaskCreate(vEthRxTask,   "eth_rx",  512,  NULL, 4, &h_eth_rx);   /* 2 KB */
    xTaskCreate(vRaftTask,    "raft",    1024, NULL, 3, &h_raft);      /* 4 KB */
    xTaskCreate(vHealthTask,  "health",  512,  NULL, 2, &h_health);    /* 2 KB */
    xTaskCreate(vMonitorTask, "monitor", 256,  NULL, 1, &h_monitor);   /* 1 KB */

    /* Create raft periodic timer */
    raft_timer = xTimerCreate("raft_tmr", pdMS_TO_TICKS(ORACLE_TICK_MS),
                              pdTRUE, NULL, vRaftTimerCallback);
    if (raft_timer)
        xTimerStart(raft_timer, 0);

    printf("heap after init: %u free\r\n",
           (unsigned)xPortGetFreeHeapSize());

    printf("starting scheduler\r\n");
    vTaskStartScheduler();

    printf("FATAL: scheduler returned\r\n");
    while (1) {}
}

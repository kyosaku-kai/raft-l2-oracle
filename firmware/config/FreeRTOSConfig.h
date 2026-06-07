/**
 * FreeRTOS configuration for raft-l2-oracle STM32F207ZG firmware.
 *
 * Decisions from docs/t2-freertos-integration-research.md:
 * - heap_4 (coalescing free), 72 KB total
 * - 1000 Hz tick (1 ms resolution for raft_periodic)
 * - ISR priority split at 5 (0-4 above FreeRTOS, 5-15 FreeRTOS-managed)
 * - TIM6 for HAL timebase, SysTick exclusively for FreeRTOS
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* Cortex-M3 specific (STM32F207ZG @ 120 MHz) */
#define configCPU_CLOCK_HZ                    ((uint32_t)120000000)
#define configTICK_RATE_HZ                    ((TickType_t)1000)
/* Do NOT define configSYSTICK_CLOCK_HZ here. When undefined, the FreeRTOS
 * CM3 port defaults to configCPU_CLOCK_HZ and sets the CLKSOURCE bit in
 * SysTick CTRL to use the processor clock (120 MHz). Defining it - even to
 * the same value - makes the port select the external reference clock
 * (AHB/8 = 15 MHz), causing an 8x tick slowdown. */

/* Scheduler */
#define configUSE_PREEMPTION                  1
#define configUSE_TIME_SLICING                1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configMAX_PRIORITIES                  8
#define configIDLE_SHOULD_YIELD               1

/* Memory */
#define configSUPPORT_DYNAMIC_ALLOCATION      1
#define configSUPPORT_STATIC_ALLOCATION       0
#define configTOTAL_HEAP_SIZE                 ((size_t)(72 * 1024))
#define configAPPLICATION_ALLOCATED_HEAP      0
#define configENABLE_HEAP_PROTECTOR           0

/* Task sizes */
#define configMINIMAL_STACK_SIZE              ((uint16_t)128)  /* 512 bytes (idle task) */
#define configMAX_TASK_NAME_LEN               12

/* Features */
#define configUSE_MUTEXES                     1
#define configUSE_COUNTING_SEMAPHORES         0
#define configUSE_RECURSIVE_MUTEXES           0
#define configUSE_QUEUE_SETS                  0
#define configUSE_TASK_NOTIFICATIONS          1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES 1
#define configUSE_TIMERS                      1
#define configTIMER_TASK_PRIORITY             (configMAX_PRIORITIES - 2)  /* priority 6 */
#define configTIMER_QUEUE_LENGTH              8
#define configTIMER_TASK_STACK_DEPTH          ((uint16_t)256)  /* 1 KB */
#define configUSE_CO_ROUTINES                 0
#define configMAX_CO_ROUTINE_PRIORITIES       1

/* Tick type */
#define configTICK_TYPE_WIDTH_IN_BITS         TICK_TYPE_WIDTH_32_BITS

/* Hook functions */
#define configUSE_IDLE_HOOK                   0
#define configUSE_TICK_HOOK                   0
#define configUSE_MALLOC_FAILED_HOOK          1
#define configCHECK_FOR_STACK_OVERFLOW        2
#define configUSE_DAEMON_TASK_STARTUP_HOOK    0

/* Runtime stats and tracing */
#define configGENERATE_RUN_TIME_STATS         0
#define configUSE_TRACE_FACILITY              1
#define configUSE_STATS_FORMATTING_FUNCTIONS  0

/* Interrupt nesting - STM32F2 uses 4 NVIC priority bits (16 levels).
 * Priorities 0-4: above FreeRTOS, cannot call ...FromISR()
 * Priorities 5-15: FreeRTOS-managed, CAN call ...FromISR()
 * Ethernet DMA interrupt must be >= 5 */
#define configPRIO_BITS                       4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY      15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY       (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY  (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* Assert */
#ifdef DEBUG
    extern void assert_failed(const char *file, int line);
    #define configASSERT(x) if ((x) == 0) assert_failed(__FILE__, __LINE__)
#else
    #define configASSERT(x) ((void)0)
#endif

/* FreeRTOS API includes */
#define INCLUDE_vTaskPrioritySet              0
#define INCLUDE_uxTaskPriorityGet             0
#define INCLUDE_vTaskDelete                   0
#define INCLUDE_vTaskSuspend                  1
#define INCLUDE_xResumeFromISR                0
#define INCLUDE_vTaskDelayUntil               1
#define INCLUDE_vTaskDelay                    1
#define INCLUDE_xTaskGetSchedulerState        1
#define INCLUDE_xTaskGetCurrentTaskHandle     1
#define INCLUDE_uxTaskGetStackHighWaterMark   1
#define INCLUDE_xTimerPendFunctionCall        0
#define INCLUDE_eTaskGetState                 0

/* Map FreeRTOS handler names to STM32 vector table names.
 * SysTick_Handler is NOT mapped here - we handle it in stm32f2xx_it.c
 * to call xPortSysTickHandler() exclusively (HAL uses TIM6 timebase). */
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler

#endif /* FREERTOS_CONFIG_H */

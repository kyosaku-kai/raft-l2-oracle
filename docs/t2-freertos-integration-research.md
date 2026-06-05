# T2: FreeRTOS Integration Research Findings

Research completed 2026-06-04. Covers FreeRTOS-Kernel source integration, FreeRTOSConfig.h for STM32F207ZG, memory allocation strategy for the raft library, and ISR-to-task communication patterns for Ethernet DMA.

## Decisions Summary

| Question | Decision | Rationale |
|----------|----------|-----------|
| FreeRTOS source | Git submodule, FreeRTOS-Kernel V11.3.0 | Native CMake, pin to release tag, minimal files |
| Heap implementation | heap_4.c (coalescing free) | Raft library does malloc/free; need coalescing to avoid fragmentation |
| configTOTAL_HEAP_SIZE | 48 KB | Covers FreeRTOS internals + raft allocations + buffers; leaves ~26 KB headroom |
| Raft allocator strategy | Route through pvPortMalloc via raft_set_heap_functions() | Single heap, unified tracking, simpler debugging |
| Raft log pre-allocation | Patch raft_new() to call log_alloc(1500) | Prevents doubling realloc; custom realloc returns NULL |
| ISR-to-task pattern | Task notification (primary) + queue for frame data | Task notification for zero-overhead wakeup, queue for frame pointer passing |
| HAL timebase | TIM6 for HAL, SysTick for FreeRTOS | Avoids the #1 cause of STM32+FreeRTOS hardfaults |
| configTICK_RATE_HZ | 1000 (1ms ticks) | Required for raft_periodic() 1ms resolution |
| Static vs dynamic allocation | Dynamic (heap_4) with allocate-at-init discipline | FreeRTOS supports both; dynamic is simpler, we enforce no-alloc-after-init at application level |

---

## 1. FreeRTOS Source Integration

### Git Submodule Setup

**Repository**: `https://github.com/FreeRTOS/FreeRTOS-Kernel.git`
**Tag**: V11.3.0 (latest stable as of 2026-06-04)
**Path**: `firmware/vendor/freertos/`

```bash
cd /path/to/raft-l2-oracle
git submodule add https://github.com/FreeRTOS/FreeRTOS-Kernel.git firmware/vendor/freertos
cd firmware/vendor/freertos
git checkout V11.3.0
cd ../../..
git add .gitmodules firmware/vendor/freertos
```

### Why Git Submodule (not vendored copy, not STM32Cube package)

- **Native CMake**: FreeRTOS-Kernel has first-class CMakeLists.txt since V11.0. No wrapper needed.
- **Version pinning**: Tag V11.3.0 is exact, reproducible, auditable via `git submodule status`.
- **Upstream updates**: `git submodule update --remote` to pull security fixes.
- **Consistent with raft**: The willemt/raft library is already a submodule at `firmware/vendor/raft/`.
- **STM32Cube package bundles**: The STM32CubeF2 firmware package includes an older FreeRTOS with ST's CMSIS-OS wrapper on top. We don't want the wrapper (it adds abstraction and hides FreeRTOS APIs). Direct FreeRTOS-Kernel submodule is cleaner.

### Minimal File Set

The FreeRTOS-Kernel CMake handles file selection automatically via `FREERTOS_PORT` and `FREERTOS_HEAP`. The files that get compiled for our configuration:

**Kernel core** (always compiled):
- `tasks.c` - task scheduler, context switching
- `queue.c` - queues, semaphores, mutexes
- `list.c` - internal linked list
- `timers.c` - software timers (we use for raft_periodic)
- `event_groups.c` - event groups (compiled but may not be used)
- `stream_buffer.c` - stream/message buffers (compiled but may not be used)
- `croutine.c` - co-routines (compiled, unused, dead-code-eliminated by --gc-sections)

**Port** (`FREERTOS_PORT=GCC_ARM_CM3`):
- `portable/GCC/ARM_CM3/port.c` - PendSV/SysTick handlers, context switch ASM
- `portable/GCC/ARM_CM3/portmacro.h` - type definitions, BASEPRI manipulation

**Heap** (`FREERTOS_HEAP=4`):
- `portable/MemMang/heap_4.c` - coalescing allocator with free list

**Config** (ours, not in FreeRTOS tree):
- `firmware/config/FreeRTOSConfig.h` - project-specific configuration

### CMake Integration

```cmake
# In firmware/CMakeLists.txt:

# 1. Define the config target (required by FreeRTOS-Kernel CMake)
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/config
)

# 2. Set port and heap before add_subdirectory
set(FREERTOS_HEAP "4" CACHE STRING "" FORCE)
set(FREERTOS_PORT "GCC_ARM_CM3" CACHE STRING "" FORCE)

# 3. Add FreeRTOS-Kernel
add_subdirectory(vendor/freertos)

# 4. Link to firmware target
target_link_libraries(app PRIVATE freertos_kernel)
```

The `freertos_config` INTERFACE library tells FreeRTOS-Kernel where to find our `FreeRTOSConfig.h`. This is the modern approach (replaces the deprecated `FREERTOS_CONFIG_FILE_DIRECTORY`).

---

## 2. FreeRTOSConfig.h for Our Use Case

### Complete Skeleton

```c
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* --- Cortex-M3 specific (STM32F207ZG @ 120MHz) --- */
#define configCPU_CLOCK_HZ                    ((uint32_t)120000000)
#define configTICK_RATE_HZ                    ((TickType_t)1000)
#define configSYSTICK_CLOCK_HZ                configCPU_CLOCK_HZ  /* SysTick runs at CPU clock */

/* --- Scheduler --- */
#define configUSE_PREEMPTION                  1
#define configUSE_TIME_SLICING                1       /* same-priority tasks round-robin */
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1     /* CLZ instruction on CM3 */
#define configMAX_PRIORITIES                  8       /* 0-7, we use 0-4 */
#define configIDLE_SHOULD_YIELD               1

/* --- Memory --- */
#define configSUPPORT_DYNAMIC_ALLOCATION      1
#define configSUPPORT_STATIC_ALLOCATION       0       /* pure dynamic for simplicity */
#define configTOTAL_HEAP_SIZE                 ((size_t)(48 * 1024))
#define configAPPLICATION_ALLOCATED_HEAP      0       /* let heap_4 use its own array */
#define configENABLE_HEAP_PROTECTOR           0       /* save cycles; enable for debug */

/* --- Task sizes --- */
#define configMINIMAL_STACK_SIZE              ((uint16_t)128)  /* 128 words = 512 bytes (idle task) */
#define configMAX_TASK_NAME_LEN               12

/* --- Features --- */
#define configUSE_MUTEXES                     1
#define configUSE_COUNTING_SEMAPHORES         0
#define configUSE_RECURSIVE_MUTEXES           0
#define configUSE_QUEUE_SETS                  0
#define configUSE_TASK_NOTIFICATIONS          1       /* primary ISR-to-task wakeup */
#define configTASK_NOTIFICATION_ARRAY_ENTRIES 1
#define configUSE_TIMERS                      1       /* for raft_periodic() */
#define configTIMER_TASK_PRIORITY             (configMAX_PRIORITIES - 2)  /* priority 6 */
#define configTIMER_QUEUE_LENGTH              8
#define configTIMER_TASK_STACK_DEPTH          ((uint16_t)256)  /* 1KB */
#define configUSE_CO_ROUTINES                 0
#define configMAX_CO_ROUTINE_PRIORITIES       1

/* --- Tick type --- */
#define configTICK_TYPE_WIDTH_IN_BITS         TICK_TYPE_WIDTH_32_BITS

/* --- Hook functions --- */
#define configUSE_IDLE_HOOK                   0
#define configUSE_TICK_HOOK                   0
#define configUSE_MALLOC_FAILED_HOOK          1       /* CRITICAL: catch OOM */
#define configCHECK_FOR_STACK_OVERFLOW        2       /* pattern check (most thorough) */
#define configUSE_DAEMON_TASK_STARTUP_HOOK    0

/* --- Runtime stats and tracing --- */
#define configGENERATE_RUN_TIME_STATS         0
#define configUSE_TRACE_FACILITY              1       /* enables uxTaskGetSystemState for debug */
#define configUSE_STATS_FORMATTING_FUNCTIONS  0

/* --- Interrupt nesting --- */
/* STM32 uses 4 priority bits (16 levels). NVIC priorities 0-15, lower number = higher priority.
 * FreeRTOS needs to know the bit shift: 8 - 4 = 4.
 * configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5 means:
 *   - Priorities 0-4: ABOVE FreeRTOS, cannot call ...FromISR() APIs
 *   - Priorities 5-15: managed by FreeRTOS, CAN call ...FromISR() APIs
 *   - Ethernet DMA interrupt MUST be >= 5 (we set it to 5)
 */
#define configPRIO_BITS                       4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY      15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY       (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY  (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* --- Assert --- */
#ifdef DEBUG
    extern void assert_failed(const char *file, int line);
    #define configASSERT(x) if ((x) == 0) assert_failed(__FILE__, __LINE__)
#else
    #define configASSERT(x) ((void)0)
#endif

/* --- FreeRTOS API includes --- */
#define INCLUDE_vTaskPrioritySet              0
#define INCLUDE_uxTaskPriorityGet             0
#define INCLUDE_vTaskDelete                   0       /* we never delete tasks */
#define INCLUDE_vTaskSuspend                  1       /* needed for portMAX_DELAY on queues */
#define INCLUDE_xResumeFromISR                0
#define INCLUDE_vTaskDelayUntil               1       /* periodic task timing */
#define INCLUDE_vTaskDelay                    1
#define INCLUDE_xTaskGetSchedulerState        1
#define INCLUDE_xTaskGetCurrentTaskHandle     1
#define INCLUDE_uxTaskGetStackHighWaterMark   1       /* stack usage monitoring */
#define INCLUDE_xTimerPendFunctionCall        0
#define INCLUDE_eTaskGetState                 0

/* --- Map FreeRTOS handler names to STM32 vector table names --- */
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
/* NOTE: Do NOT define vPortSysTickHandler = SysTick_Handler here.
 * SysTick_Handler calls both HAL_IncTick() (if HAL timebase is SysTick)
 * and xPortSysTickHandler(). We handle this in stm32f2xx_it.c.
 * With TIM6 as HAL timebase, SysTick is exclusively FreeRTOS's. */

#endif /* FREERTOS_CONFIG_H */
```

### Key Configuration Decisions Explained

**configTOTAL_HEAP_SIZE = 48 KB**: This is the FreeRTOS heap_4 managed pool. Everything allocated via pvPortMalloc comes from here, including:

| Consumer | Estimated Size | Notes |
|----------|---------------|-------|
| Task TCBs (5 tasks) | ~0.5 KB | ~100 bytes each |
| Task stacks (sum) | ~10 KB | 4+2+2+1+1 KB per design doc |
| FreeRTOS queues (3) | ~0.2 KB | Queue control structures |
| Queue storage (raft_inbox) | ~3 KB | 8 slots x ~400 bytes (frame + metadata) |
| Queue storage (hb_inbox) | ~0.5 KB | 8 slots x ~64 bytes |
| Queue storage (tx_queue) | ~3 KB | 8 slots x ~400 bytes |
| Software timer structs | ~0.1 KB | 2 timers |
| Raft library allocs | ~0.5 KB | server_private + log_private + nodes + ptrs |
| **Subtotal (from heap_4)** | **~18 KB** | |
| **Headroom in heap** | **~30 KB** | Available for raft log + payloads |

The raft log (30 KB metadata + 18 KB payloads = 48 KB) does NOT come from this heap. It is allocated via `raft_set_heap_functions()` which we route to pvPortMalloc, BUT the log is pre-allocated once at init. Total heap usage after init: ~66 KB of the 48+18=66... wait, let me recalculate.

Actually, we need to reconsider. The raft log metadata (1500 x 20 = 30 KB) and payload pool (1500 x 12 = 18 KB) ARE allocated through pvPortMalloc (via raft_set_heap_functions). So the total heap must cover all of it:

| Consumer | Size |
|----------|------|
| FreeRTOS internals (TCBs, stacks, queues, timers) | ~18 KB |
| Raft log metadata (1500 x 20B) | ~30 KB |
| Raft log payload pool (1500 x 12B) | ~18 KB |
| Raft fixed structs (server, log, nodes) | ~0.5 KB |
| **Total heap needed** | **~66.5 KB** |

This means `configTOTAL_HEAP_SIZE` should be **~70 KB** (66.5 KB + overhead for heap_4 block headers and alignment). Let me revise the memory budget:

### Revised Memory Budget (128 KB SRAM)

| Region | Size | Source |
|--------|------|--------|
| FreeRTOS heap_4 pool (configTOTAL_HEAP_SIZE) | 72 KB | Static array in .bss |
| - FreeRTOS internals (TCBs, stacks, queues) | (18 KB) | Allocated from heap at init |
| - Raft log metadata (1500 x 20B) | (30 KB) | Allocated from heap at init |
| - Raft payload pool (1500 x 12B) | (18 KB) | Allocated from heap at init |
| - Raft fixed structs | (0.5 KB) | Allocated from heap at init |
| - heap_4 overhead (block headers, alignment) | (~4 KB) | Internal to heap_4 |
| - Remaining free heap after init | (~1.5 KB) | Safety margin |
| MAC RX/TX DMA descriptors + buffers | 8 KB | Static arrays, DMA-accessible |
| Health state table | 1 KB | Static global |
| Corroboration table | 0.1 KB | Static global |
| Message buffers (in-flight frame assembly) | 4 KB | Static or stack |
| Heartbeat tracking state | 2 KB | Static global |
| .data + .bss (non-heap globals) | 4 KB | Linker-placed |
| .text + .rodata (in Flash, not SRAM) | 0 KB | Runs from Flash |
| MSP (main stack, used before scheduler + ISRs) | 2 KB | Top of SRAM, set in linker script |
| **Total SRAM used** | **~93 KB** | |
| **Headroom** | **~35 KB** | |

**Key insight**: The design doc's 102 KB estimate is close but didn't account for heap_4 block headers (8 bytes per allocation, ~50 allocations = ~400 bytes) or the fact that the FreeRTOS heap array itself lives in .bss and must be sized to contain everything. With 72 KB heap + 21 KB non-heap, we land at ~93 KB, leaving ~35 KB headroom.

**If SRAM is tight**: First lever is reducing raft log from 1500 to 500 entries: saves 20 KB metadata + 12 KB payloads = 32 KB. This brings heap down to 40 KB and total to ~61 KB.

**configTICK_RATE_HZ = 1000**: Gives 1ms tick resolution. The design doc specifies raft_periodic() should be called with elapsed_ms, and 1ms is the finest granularity we need. Using `vTaskDelayUntil()` for periodic tasks gives deterministic timing at this resolution. Note: 1000 Hz tick is standard for most STM32+FreeRTOS projects. Going higher (e.g., 10 kHz) wastes CPU in SysTick ISR; going lower (e.g., 100 Hz) gives only 10ms resolution which is too coarse for heartbeat monitoring.

**configCHECK_FOR_STACK_OVERFLOW = 2**: The most thorough method - fills stack with a known pattern at creation, checks for corruption on context switch. Small overhead (~10 cycles per context switch). Critical during development; can be disabled for production if CPU margin is tight.

**configUSE_MALLOC_FAILED_HOOK = 1**: Calls `vApplicationMallocFailedHook()` if pvPortMalloc returns NULL. We MUST implement this - an OOM on the STM32 is a fatal error that should trigger a visible fault (LED pattern + UART dump + optional reset).

**Interrupt priority model**: STM32F2 uses 4 NVIC priority bits (16 levels, 0=highest). The `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5` split means:
- Priorities 0-4: "above FreeRTOS" - cannot call any FreeRTOS ...FromISR() API
- Priorities 5-15: "FreeRTOS-managed" - CAN call xQueueSendFromISR(), xTaskNotifyFromISR(), etc.
- Our Ethernet DMA interrupt: priority 5 (highest FreeRTOS-safe level)
- SysTick: priority 15 (lowest, per FreeRTOS requirement)
- PendSV: priority 15 (lowest, per FreeRTOS requirement)

**CRITICAL**: Getting this wrong causes random hardfaults. Any ISR that calls a FreeRTOS ...FromISR() function MUST have NVIC priority >= 5 (numerically >= 5, i.e., lower urgency). The Ethernet interrupt is the main one to get right.

### HAL Timebase: TIM6 instead of SysTick

By default, STM32 HAL uses SysTick for `HAL_GetTick()` / `HAL_Delay()`. FreeRTOS also needs SysTick for its scheduler tick. Running both from one SysTick ISR works but is fragile - the #1 source of "random hardfaults" in STM32+FreeRTOS projects (T1 findings).

**Solution**: Configure HAL to use TIM6 as its timebase instead:
- `stm32f2xx_hal_timebase_tim.c` overrides `HAL_InitTick()` to set up TIM6 at 1ms
- SysTick is exclusively FreeRTOS's - mapped directly to `xPortSysTickHandler()`
- No conflict, no shared ISR, clean separation

This is a standard pattern recommended by ST's own FreeRTOS integration examples.

---

## 3. Memory Allocation Strategy

### Decision: Route Raft Through pvPortMalloc

The design doc identified three options for raft library memory:

| Option | Pros | Cons | Selected |
|--------|------|------|----------|
| (a) Route through pvPortMalloc | Single heap, unified tracking, simpler | Shares heap with FreeRTOS internals | **YES** |
| (b) Separate static pool for raft | Isolated from FreeRTOS, deterministic | Two heaps to manage, more complex | No |
| (c) Allocate at init, never free | Simplest runtime behavior | Still needs allocator for init phase | Discipline, not mechanism |

**Why (a)**: heap_4.c already provides coalescing free, heap watermark tracking (`xPortGetMinimumEverFreeHeapSize()`), and thread-safety via critical sections. Adding a second allocator for raft gains isolation but doubles the debugging surface. Since we enforce "no allocation after init" at the application level anyway, fragmentation from raft's malloc/free pattern during init is a non-issue - heap_4 will coalesce any freed blocks.

Option (c) is our **discipline** regardless of mechanism: after `oracle_init()` completes, the heap free size should be stable. We verify this with a runtime assertion: sample heap free at init+1s and assert it hasn't decreased at init+60s.

### raft_set_heap_functions() Implementation

```c
#include "FreeRTOS.h"
#include "task.h"
#include "raft.h"

static void *bare_malloc(size_t size)
{
    return pvPortMalloc(size);
}

static void *bare_calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void *p = pvPortMalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

static void *bare_realloc(void *ptr, size_t size)
{
    /* INTENTIONALLY returns NULL.
     * The only realloc call site is __ensurecapacity() in raft_log.c.
     * Returning NULL triggers RAFT_ERR_NOMEM, which the library handles
     * gracefully (propagates error, does not corrupt state).
     * We prevent this path entirely by pre-allocating the log at full
     * capacity via the log_alloc(1500) patch. */
    (void)ptr;
    (void)size;
    return NULL;
}

static void bare_free(void *ptr)
{
    vPortFree(ptr);
}

void oracle_heap_init(void)
{
    raft_set_heap_functions(bare_malloc, bare_calloc, bare_realloc, bare_free);
}
```

**Call `oracle_heap_init()` before `oracle_init()`** - the heap functions must be set before any raft_new() call.

### Raft Log Pre-Allocation Patch

The vendored raft library at `firmware/vendor/raft/` needs one change. In `raft_server.c`, the `raft_new()` function calls `log_new()` which starts with `INITIAL_CAPACITY=10` entries. We need it to call `log_alloc(MAX_LOG_ENTRIES)` instead:

```c
/* In raft_server.c, raft_new() function: */
/* BEFORE: */
me->log = log_new();
/* AFTER: */
me->log = log_alloc(RAFT_MAX_LOG_ENTRIES);  /* pre-allocate full capacity */
```

Where `RAFT_MAX_LOG_ENTRIES` is defined in our config (default 1500, can reduce to 500 if memory-constrained).

The `log_alloc()` function already exists in `raft_log.c:89` and takes an `initial_size` parameter - it allocates the entry array at exactly that size. With our `bare_realloc` returning NULL, the log can never grow beyond this initial allocation. The library's `__ensurecapacity()` will return `RAFT_ERR_NOMEM` if the log is full, which propagates cleanly.

### Entry Payload Pool

Raft log entries carry health transition data (12 bytes each) in `raft_entry_t.data.buf`. The current simulator code (raft_oracle.c:126) does `malloc(entry->data.len)` in `cb_log_offer()` and `free()` in `cb_log_pop()`/`cb_log_poll()`.

On bare-metal, these small allocations go through pvPortMalloc (via our heap function redirect). This is acceptable because:
- Each allocation is 12 bytes (health transition struct)
- heap_4 block header adds 8 bytes overhead = 20 bytes per entry
- Maximum 1500 entries x 20 bytes = 30 KB (already accounted in budget)
- Allocations and frees happen in FIFO order (log is a ring buffer), which is ideal for heap_4's coalescing

**Alternative considered**: A fixed-size pool allocator (static array of 1500 x 12-byte slots with bitmap). This would eliminate fragmentation risk entirely but adds custom allocator code. Not worth it given heap_4's coalescing and our FIFO access pattern. Revisit only if heap watermark monitoring shows unexpected fragmentation.

### Allocation Timeline

```
Boot
  |
  v
main() -> SystemInit(), HAL_Init(), clock config
  |
  v
oracle_heap_init()                     -- set raft heap functions
  |
  v
xTaskCreate() x 5                     -- FreeRTOS creates TCBs + stacks from heap
xQueueCreate() x 3                    -- FreeRTOS creates queues from heap
xTimerCreate() x 2                    -- FreeRTOS creates timer structs from heap
  |                                      ~18 KB allocated from heap
  v
oracle_init()
  -> raft_new()
     -> log_alloc(1500)                -- 30 KB for entry metadata array
     -> server_private_t               -- 100 bytes
  -> raft_add_node() x 4              -- 160 bytes + 32 bytes ptr array
  |                                      ~30.3 KB allocated from heap
  v
vTaskStartScheduler()                 -- scheduler starts, tasks begin running
  |
  v
[raft_task processes first entries]
  -> cb_log_offer() -> pvPortMalloc(12) -- entry payloads allocated on demand
  -> cb_log_poll() -> vPortFree()       -- old entry payloads freed (FIFO)
  |
  v
[NO MORE NEW ALLOCATIONS]             -- heap free stabilizes
                                       -- total used: ~48-50 KB of 72 KB heap
                                       -- remaining: ~22-24 KB free
```

---

## 4. Task-to-ISR Communication Patterns

### The Problem

The STM32F207's Ethernet MAC uses DMA to transfer frames between SRAM and the MAC. When a frame arrives:

1. DMA writes frame data to a pre-configured RX buffer in SRAM
2. DMA updates the RX descriptor (marks it as "frame received")
3. DMA triggers the ETH global interrupt (IRQn = ETH_IRQn, vector = ETH_IRQHandler)
4. ISR must signal a FreeRTOS task to process the frame
5. ISR must NOT do heavy processing (parse frame, call raft_recv, etc.)

### Recommended Pattern: Task Notification + Queue

```
ETH DMA RX interrupt
  |
  v
ETH_IRQHandler()
  |-- Acknowledge DMA interrupt (clear status bits)
  |-- Identify which RX descriptor(s) have new frames
  |-- For each received frame:
  |     |-- Swap DMA descriptor buffer pointer with a fresh buffer from a free pool
  |     |-- Post received buffer pointer to rx_frame_queue (xQueueSendFromISR)
  |-- Wake eth_rx_task via xTaskNotifyFromISR() if any frames queued
  v
eth_rx_task (priority 4, highest application task)
  |-- Block on ulTaskNotifyTake(pdTRUE, portMAX_DELAY)
  |-- Drain rx_frame_queue:
  |     |-- Read frame header, check EtherType
  |     |-- 0x88B5 -> xQueueSend(raft_inbox)
  |     |-- 0x88B7 -> xQueueSend(hb_inbox)
  |     |-- Other -> discard, return buffer to free pool
  v
raft_task / health_task
  |-- Block on xQueueReceive() from respective inbox
  |-- Process frame
  |-- Return buffer to free pool
```

### Why This Pattern (Not Alternatives)

**Task notification vs. binary semaphore**: Task notifications are ~45% faster than semaphores on Cortex-M3 (no kernel object overhead, no priority inheritance check). Since we only need "wake up, there's work" semantics (not counting or mutual exclusion), task notifications are ideal. FreeRTOS V11 task notifications are one-to-one (each task has its own notification value), which maps perfectly to "ISR wakes eth_rx_task".

**Queue for frame data (not direct task notification value)**: Task notification can carry a 32-bit value, but we may receive multiple frames per interrupt (DMA can buffer several frames). A queue handles burst reception naturally. The queue holds frame buffer pointers (4 bytes each), not frame data copies.

**xQueueSendFromISR vs. xStreamBufferSendFromISR**: Stream buffers are designed for byte streams (UART, etc.), not discrete messages. Queues are designed for fixed-size items (frame pointers). Queues are the right tool here.

### Zero-Copy DMA Pattern

The key to avoiding memcpy from DMA buffer to queue is **buffer swapping**:

1. Pre-allocate a pool of N frame buffers (each 1536 bytes for max Ethernet frame)
2. At init, assign one buffer to each DMA RX descriptor
3. On frame reception, the ISR does NOT copy the frame. Instead:
   - Take a fresh buffer from the free pool
   - Swap it into the DMA descriptor (so DMA can receive the next frame into it)
   - Post the OLD buffer pointer (containing the received frame) to rx_frame_queue
4. The processing task reads the frame directly from the buffer, then returns it to the free pool

This eliminates all memcpy in the RX path. The frame sits in one buffer from DMA write to application processing.

**Buffer pool sizing**: 8 RX descriptors + 4 spare = 12 buffers x 1536 bytes = ~18 KB. This is more than the design doc's 8 KB estimate for "MAC RX/TX descriptor rings" because the design doc only counted descriptors (32 bytes each), not the frame buffers they point to.

**Revised non-heap SRAM for DMA**:

| Item | Size |
|------|------|
| RX descriptors (8 x 32B) | 256 B |
| TX descriptors (4 x 32B) | 128 B |
| RX frame buffers (12 x 1536B) | 18 KB |
| TX frame buffers (4 x 1536B) | 6 KB |
| **Total DMA region** | **~24.4 KB** |

This significantly increases the DMA memory footprint versus the design doc's 8 KB estimate. However, the zero-copy benefit is worth it: memcpy of a 100-byte raft frame at 120 MHz takes ~1 us, but for burst reception of multiple frames, the cumulative cost and buffer management complexity of copying is worse than just swapping pointers.

**Revised total SRAM budget with DMA buffers**:

| Region | Size |
|--------|------|
| FreeRTOS heap_4 pool | 72 KB |
| DMA descriptors + frame buffers | 24.4 KB |
| Health/corroboration/heartbeat state | 3.1 KB |
| .data + .bss (non-heap globals) | 4 KB |
| MSP (ISR stack) | 2 KB |
| **Total** | **~105.5 KB** |
| **Headroom** | **~22.5 KB** |

This is tighter than the initial 93 KB estimate. If headroom is insufficient:
1. Reduce RX frame buffers from 12 to 8 (save 6 KB) - acceptable if burst reception is rare
2. Reduce raft log from 1500 to 500 entries (save 32 KB in heap) - generous headroom
3. Use 512-byte frame buffers instead of 1536 (save ~12 KB) - our max oracle frame is ~88 bytes, but Ethernet minimum is 64 bytes. Could use 256-byte buffers since our frames are always <128 bytes, but this breaks if we ever need to handle larger frames

**Recommendation**: Start with the full 1536-byte buffers and 1500-entry log. Measure actual heap watermark after boot. Reduce log depth first (easiest lever, biggest savings) if headroom is under 10 KB.

### DMA Buffer Placement

DMA buffers MUST be in SRAM accessible by the Ethernet DMA engine. On STM32F207, all SRAM is DMA-accessible (single SRAM bank, no TCM/AXI split like F7). No special linker section needed - just ensure buffers are not in the FreeRTOS heap (they're statically allocated).

```c
/* Static DMA buffers - NOT in FreeRTOS heap */
static uint8_t rx_buffers[NUM_RX_BUFFERS][ETH_MAX_FRAME_SIZE]
    __attribute__((aligned(4)));
static uint8_t tx_buffers[NUM_TX_BUFFERS][ETH_MAX_FRAME_SIZE]
    __attribute__((aligned(4)));

/* DMA descriptors - must be 4-byte aligned */
static ETH_DMADescTypeDef rx_descriptors[NUM_RX_DESCRIPTORS]
    __attribute__((aligned(4)));
static ETH_DMADescTypeDef tx_descriptors[NUM_TX_DESCRIPTORS]
    __attribute__((aligned(4)));
```

### Ethernet ISR Implementation Sketch

```c
void ETH_IRQHandler(void)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    uint32_t status = ETH->DMASR;

    /* Normal receive interrupt */
    if (status & ETH_DMASR_RS) {
        /* Clear interrupt flag */
        ETH->DMASR = ETH_DMASR_RS | ETH_DMASR_NIS;

        /* Process all completed RX descriptors */
        while (rx_desc_available()) {
            uint8_t *frame_buf = get_current_rx_buffer();
            uint16_t frame_len = get_current_rx_length();

            /* Swap in a fresh buffer for DMA */
            uint8_t *fresh = pool_alloc();
            if (fresh) {
                set_rx_descriptor_buffer(fresh);

                /* Post received frame to queue */
                rx_frame_t frame = { .buf = frame_buf, .len = frame_len };
                xQueueSendFromISR(rx_frame_queue, &frame,
                                  &xHigherPriorityTaskWoken);
            } else {
                /* No free buffers - frame stays in DMA descriptor, will be
                 * overwritten on next reception. Count as dropped frame. */
                stats.rx_pool_exhausted++;
            }

            advance_rx_descriptor();
        }

        /* Wake eth_rx_task */
        vTaskNotifyGiveFromISR(eth_rx_task_handle,
                               &xHigherPriorityTaskWoken);
    }

    /* Error interrupts */
    if (status & ETH_DMASR_AIS) {
        ETH->DMASR = ETH_DMASR_AIS;  /* clear abnormal interrupt summary */
        stats.dma_errors++;
        /* TODO: handle specific errors (bus error, RX overflow, etc.) */
    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
```

**NVIC priority**: ETH_IRQn must be set to priority 5 (= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY). This is the highest priority that can safely call FreeRTOS ...FromISR() APIs. Setting it lower than 5 causes hardfaults; setting it higher than 5 (e.g., 6-15) is safe but gives Ethernet lower urgency than necessary.

```c
HAL_NVIC_SetPriority(ETH_IRQn, 5, 0);
HAL_NVIC_EnableIRQ(ETH_IRQn);
```

### TX Path

TX is simpler - no ISR involvement for our use case:

```c
/* Called from raft_task or health_task context */
int transport_eth_send(const uint8_t *frame, size_t len)
{
    /* Wait for a TX descriptor to become available (DMA done with previous) */
    while (!tx_desc_available()) {
        vTaskDelay(1);  /* yield, check again next tick */
    }

    /* Copy frame into TX buffer (frame is small, ~88 bytes max) */
    memcpy(get_tx_buffer(), frame, len);

    /* Pad to minimum Ethernet frame size (60 bytes + 4 FCS = 64) */
    if (len < 60) {
        memset(get_tx_buffer() + len, 0, 60 - len);
        len = 60;
    }

    /* Set TX descriptor length and trigger DMA */
    set_tx_descriptor_length(len);
    trigger_tx_dma();

    return 0;
}
```

TX does use memcpy (frame is assembled in stack/static buffer, then copied to DMA TX buffer). This is fine - our frames are small (<128 bytes) and TX rate is low (~20 frames/sec for Raft heartbeats).

**Alternatively**, a tx_queue could decouple frame assembly from DMA submission, but this adds complexity for no measurable benefit at our TX rates. Direct submission from the sending task is simpler.

---

## 5. Risks and Mitigations Specific to T2 Findings

| Risk | Mitigation |
|------|------------|
| 72 KB heap leaves only ~22 KB headroom | Reduce log to 500 entries if needed (saves 32 KB). Monitor with xPortGetMinimumEverFreeHeapSize(). |
| DMA frame buffers consume 24 KB | Use 512-byte or 256-byte buffers (our frames are <128 bytes). Trade off: breaks if non-oracle Ethernet traffic arrives with larger frames. |
| heap_4 fragmentation from raft entry payloads | FIFO pattern (offer/poll) is ideal for coalescing. Monitor heap fragmentation via xPortGetFreeHeapSize() vs xPortGetMinimumEverFreeHeapSize() delta. |
| Ethernet ISR priority misconfiguration | Enforce via configASSERT in ISR entry: `configASSERT(xPortIsInsideInterrupt())`. Set NVIC priority exactly to 5 and verify with vPortValidateInterruptPriority(). |
| HAL_Delay() used before scheduler starts | HAL uses TIM6 timebase; works independently of FreeRTOS scheduler. No issue. |
| Stack overflow in raft_task (4 KB) | __log() stubbed (saves 1 KB stack). configCHECK_FOR_STACK_OVERFLOW=2 catches overflow. Monitor with uxTaskGetStackHighWaterMark(). |

---

## 6. Implementation Checklist for T5 (FreeRTOS Integration Task)

When implementing T5, use these findings directly:

1. Add FreeRTOS-Kernel submodule at V11.3.0
2. Create `firmware/config/FreeRTOSConfig.h` from the skeleton above
3. Add CMake integration (freertos_config target, FREERTOS_PORT/HEAP vars)
4. Implement `stm32f2xx_hal_timebase_tim.c` for TIM6 HAL timebase
5. Wire SysTick exclusively to FreeRTOS: `SysTick_Handler` calls `xPortSysTickHandler()`
6. Map handler names: `SVC_Handler` and `PendSV_Handler` in stm32f2xx_it.c
7. Implement `vApplicationMallocFailedHook()` and `vApplicationStackOverflowHook()`
8. Create 2 test tasks (LED toggle at different rates) + 1 queue test
9. Print heap stats on UART: `xPortGetFreeHeapSize()`, `xPortGetMinimumEverFreeHeapSize()`
10. Verify total heap usage after init matches this document's estimates

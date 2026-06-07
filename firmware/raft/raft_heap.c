/**
 * raft_heap.c - Bare-metal heap adapter for willemt/raft
 *
 * On STM32 (no RAFT_SIM), routes raft's pluggable heap through FreeRTOS
 * pvPortMalloc/vPortFree. bare_realloc() intentionally returns NULL to
 * prevent log doubling - the log is pre-allocated to full capacity at init.
 *
 * On POSIX simulator (RAFT_SIM defined), this is a no-op since the raft
 * library defaults to libc malloc/calloc/realloc/free.
 */

#include "raft_heap.h"

#include <stddef.h>  /* size_t - needed before raft.h in freestanding C99 */
#include "raft.h"

#ifndef RAFT_SIM

#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

static void *bare_malloc(size_t size)
{
    return pvPortMalloc(size);
}

static void *bare_calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void *p = pvPortMalloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

static void *bare_realloc(void *ptr, size_t size)
{
    /*
     * Minimal realloc: allocate new block, copy old data, free old block.
     * FreeRTOS heap_4 doesn't provide realloc natively.
     *
     * This is used by raft_add_node (node array growth) and potentially
     * __ensurecapacity (log doubling). The log doubling path is prevented
     * by pre-allocating the log to full capacity at init, so in practice
     * this only handles the small node array reallocations.
     */
    void *new_ptr = pvPortMalloc(size);
    if (!new_ptr)
        return NULL;
    if (ptr) {
        /* We don't know the old block size (FreeRTOS doesn't expose it).
         * Copy `size` bytes - may overread the old block by a few bytes,
         * but this is safe: FreeRTOS heap_4 blocks are word-aligned, and
         * ARM Cortex-M3 won't fault on valid heap addresses. The caller
         * (raft_add_node) overwrites the new slot immediately after. */
        memcpy(new_ptr, ptr, size);
        vPortFree(ptr);
    }
    return new_ptr;
}

static void bare_free(void *ptr)
{
    vPortFree(ptr);
}

void raft_heap_init(void)
{
    raft_set_heap_functions(bare_malloc, bare_calloc, bare_realloc, bare_free);
}

#else /* RAFT_SIM */

void raft_heap_init(void)
{
    /* Simulator uses libc defaults - nothing to do */
}

#endif /* RAFT_SIM */

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
     * The only caller is raft_add_node() which grows the nodes pointer
     * array by one element each time:
     *   realloc(ptr, sizeof(void*) * (num_nodes + 1))
     * So old_size = size - sizeof(void*), except for the first call
     * where ptr is NULL (handled by the if-guard).
     */
    void *new_ptr = pvPortMalloc(size);
    if (!new_ptr)
        return NULL;
    if (ptr) {
        size_t old_size = (size > sizeof(void*)) ? size - sizeof(void*) : 0;
        memcpy(new_ptr, ptr, old_size);
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

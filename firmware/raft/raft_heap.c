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
     * Intentionally returns NULL. The only realloc call site is
     * __ensurecapacity() in raft_log.c which doubles the log array.
     * Returning NULL triggers RAFT_ERR_NOMEM, handled gracefully by
     * the library (error propagates, no state corruption).
     *
     * We prevent this path by pre-allocating the log to full capacity
     * via the log_alloc(ORACLE_MAX_LOG_ENTRIES) patch in raft_server.c.
     */
    (void)ptr;
    (void)size;
    return NULL;
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

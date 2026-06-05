#ifndef RAFT_HEAP_H
#define RAFT_HEAP_H

/**
 * raft_heap.h - Bare-metal heap adapter for willemt/raft
 *
 * Routes raft's pluggable allocator through FreeRTOS pvPortMalloc/vPortFree
 * on STM32, or through standard libc on POSIX (simulator). Must be called
 * before oracle_init().
 */

/**
 * Install bare-metal allocators via raft_set_heap_functions().
 * On firmware: pvPortMalloc/vPortFree wrappers with NULL-returning realloc.
 * On sim: no-op (raft defaults to libc malloc/free).
 */
void raft_heap_init(void);

#endif /* RAFT_HEAP_H */

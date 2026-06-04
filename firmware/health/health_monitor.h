#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

/**
 * health_monitor.h - Local compute node health monitoring
 *
 * Each STM32 monitors its local compute nodes (Jetson, x86) via
 * L2 heartbeats (EtherType 0x88B7). When heartbeats are missed,
 * the monitor proposes state transitions via the Raft oracle.
 */

#include <stdint.h>

/** Node health states */
typedef enum {
    NODE_UP      = 0,
    NODE_SUSPECT = 1,
    NODE_DOWN    = 2,
} node_status_t;

/** Compute node types */
typedef enum {
    NODE_TYPE_JETSON = 1,
    NODE_TYPE_X86    = 2,
    NODE_TYPE_STM32  = 3,
} node_type_t;

/** Health table entry - replicated via Raft */
typedef struct {
    uint8_t  node_id;
    uint8_t  box_id;
    uint8_t  node_type;      /* node_type_t */
    uint8_t  status;         /* node_status_t */
    uint32_t last_seen_ms;
    uint32_t status_term;    /* Raft term when status last changed */
} node_health_entry_t;

/** Maximum tracked nodes across all boxes */
#define MAX_HEALTH_NODES 16

/* TODO: Implement health_monitor_init(), health_monitor_tick(),
 * health_monitor_process_heartbeat(), health_monitor_get_table() */

#endif /* HEALTH_MONITOR_H */

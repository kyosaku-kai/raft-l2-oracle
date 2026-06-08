#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

/**
 * health_monitor.h - Local compute node health monitoring
 *
 * Each STM32 monitors its local compute nodes (Jetson, x86) via
 * L2 heartbeats (EtherType 0x88B7). When heartbeats are missed,
 * the monitor proposes state transitions via the Raft oracle.
 *
 * State machine (design doc Section 6):
 *   [*] -> UP         MSG_NODE_ANNOUNCE received
 *   UP -> SUSPECT     3 missed heartbeats (30ms at 10ms interval)
 *   SUSPECT -> DOWN   50ms no recovery (leader checks corroboration first)
 *   SUSPECT -> UP     heartbeat resumes
 *   DOWN -> UP        heartbeat resumes
 */

#include <stdint.h>
#include <stddef.h>

#include "../protocol/wire_format.h"
#include "../transport/transport.h"

/* Forward declaration to avoid circular dependency with raft_oracle.h */
struct oracle_node_ctx;

/* --- Node health states (values from wire_format.h) --- */

typedef enum {
    NODE_UP      = NODE_STATUS_UP,
    NODE_SUSPECT = NODE_STATUS_SUSPECT,
    NODE_DOWN    = NODE_STATUS_DOWN,
} node_status_t;

/* --- Timing constants --- */

#define HB_INTERVAL_MS       10   /* expected heartbeat period */
#define SUSPECT_THRESHOLD     3   /* missed heartbeats before SUSPECT */
#define DOWN_TIMEOUT_MS      50   /* ms in SUSPECT before DOWN */
#define STALE_OBS_MS        500   /* discard observations older than this */
#define CORROBORATION_K       2   /* observers needed for cross-box DOWN */

/* --- Health table entry (replicated via Raft) --- */

typedef struct {
    uint8_t  node_id;
    uint8_t  box_id;
    uint8_t  node_type;      /* node_type_t */
    uint8_t  status;         /* node_status_t */
    uint32_t last_seen_ms;
    uint32_t status_term;    /* Raft term when status last changed */
} node_health_entry_t;

#define MAX_HEALTH_NODES 16

/* Need ORACLE_MAX_NODES from raft_oracle.h but can't include it (circular).
 * Use the same value (8) via a compile-time check in health_monitor.c. */
#ifndef ORACLE_MAX_NODES
#define ORACLE_MAX_NODES 8
#endif

/* --- Corroboration table (leader-side, ~68 bytes) --- */

typedef struct {
    uint8_t  status[ORACLE_MAX_NODES][MAX_HEALTH_NODES]; /* per-peer, per-node */
    uint32_t updated_ms[ORACLE_MAX_NODES];               /* when peer last reported */
} corroboration_table_t;

/* --- Health monitor context --- */

typedef struct {
    /* Replicated health table */
    node_health_entry_t table[MAX_HEALTH_NODES];
    uint8_t             n_nodes;

    /* Per-node miss tracking (parallel to table[]) */
    uint16_t            miss_count[MAX_HEALTH_NODES];
    uint32_t            suspect_since_ms[MAX_HEALTH_NODES];
    uint32_t            last_tick_ms;    /* for delta-based miss detection */

    /* Corroboration (leader-side) */
    corroboration_table_t corroboration;

    /* Back-references */
    struct oracle_node_ctx *oracle;
    raft_transport_t       *transport;
    uint8_t                 box_id;
} health_monitor_t;

/* --- API --- */

/**
 * Initialize the health monitor. Call once at startup.
 */
void health_monitor_init(health_monitor_t *hm,
                         struct oracle_node_ctx *oracle,
                         raft_transport_t *transport,
                         uint8_t box_id);

/**
 * Process an incoming heartbeat. Updates last_seen, resets miss counter.
 * If node was SUSPECT/DOWN, proposes UP transition.
 */
void health_monitor_process_heartbeat(health_monitor_t *hm,
                                      uint8_t src_node,
                                      const payload_node_heartbeat_t *hb,
                                      uint32_t now_ms);

/**
 * Process a node announcement. Registers node in table as UP.
 * Sends MSG_NODE_ANNOUNCE_ACK back.
 */
void health_monitor_process_announce(health_monitor_t *hm,
                                     uint8_t src_node,
                                     const payload_node_announce_t *ann,
                                     uint32_t now_ms);

/**
 * Periodic tick - call every ~10ms. Drives miss detection and
 * state transitions (UP->SUSPECT->DOWN).
 */
void health_monitor_tick(health_monitor_t *hm, uint32_t now_ms);

/**
 * Get current health observations for AE response sideband.
 * Fills out[] with up to MAX_OBSERVATIONS_PER_RESP entries.
 * Sets *n_out to the number of entries written.
 */
void health_monitor_get_observations(health_monitor_t *hm,
                                     observation_entry_t *out,
                                     uint8_t *n_out);

/**
 * Leader-side: update corroboration table with follower observations
 * from an AE response.
 */
void health_monitor_update_corroboration(health_monitor_t *hm,
                                         uint8_t peer_id,
                                         const observation_entry_t *obs,
                                         uint8_t n_obs,
                                         uint32_t now_ms);

/**
 * Apply a committed health transition from the Raft log.
 * Called from cb_applylog(). Updates table and emits 0x88B6 health event.
 */
void health_table_apply(health_monitor_t *hm,
                        uint8_t target_node, uint8_t target_box,
                        uint8_t old_status, uint8_t new_status,
                        uint32_t term);

/**
 * Broadcast MSG_CLUSTER_STATE (0x88B6 0x11) to local compute nodes.
 * Leader-only; call every ~1 second from the raft task.
 * oracle-agent depends on receiving this for its safety interlock.
 */
void health_table_broadcast_cluster_state(health_monitor_t *hm,
                                          struct oracle_node_ctx *ctx);

/**
 * Get the health table for display/debugging.
 */
const node_health_entry_t *health_monitor_get_table(const health_monitor_t *hm,
                                                    uint8_t *n_nodes);

#endif /* HEALTH_MONITOR_H */

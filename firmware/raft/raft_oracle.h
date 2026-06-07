#ifndef RAFT_ORACLE_H
#define RAFT_ORACLE_H

/**
 * raft_oracle.h - High-level Raft oracle wrapper around willemt/raft
 *
 * Provides initialization, callback registration, and message dispatch
 * for the L2 failure detection oracle. Bridges the willemt/raft library
 * to our transport HAL and health monitoring subsystem.
 */

#include <stddef.h>  /* size_t, needed by raft.h with strict compilers */
#include "raft.h"
#include "../transport/transport.h"
#include "../protocol/wire_format.h"

#include <stdint.h>

/** Maximum nodes in the oracle cluster (STM32s only, not compute nodes) */
#define ORACLE_MAX_NODES 8

/** Maximum log entries before compaction */
#define ORACLE_MAX_LOG_ENTRIES 1500

/** Raft periodic tick interval in milliseconds */
#define ORACLE_TICK_MS 50

/** Per-node context passed as user_data to raft callbacks */
typedef struct oracle_node_ctx {
    raft_server_t      *raft;
    raft_transport_t   *transport;
    uint8_t             node_id;
    uint8_t             box_id;

    /* RAM-only persistence (v1 - no flash) */
    raft_term_t         persisted_term;
    raft_node_id_t      persisted_vote;

    /* Entry ID counter */
    int                 next_entry_id;

    /* Health monitor (set after health_monitor_init, NULL until then) */
    void               *health_ctx;
} oracle_node_ctx_t;

/**
 * Initialize an oracle node.
 * Must call raft_set_heap_functions() before this if using custom allocator.
 *
 * @param ctx       Context to initialize (caller-owned)
 * @param node_id   This node's unique ID
 * @param box_id    This node's box ID
 * @param transport Transport implementation to use
 * @return 0 on success, -1 on error
 */
int oracle_init(oracle_node_ctx_t *ctx, uint8_t node_id, uint8_t box_id,
                raft_transport_t *transport);

/**
 * Add a peer node to the cluster.
 *
 * @param ctx       This node's context
 * @param peer_id   Peer's node ID
 * @param is_self   1 if this is the local node, 0 for peers
 * @return 0 on success
 */
int oracle_add_node(oracle_node_ctx_t *ctx, uint8_t peer_id, int is_self);

/**
 * Process one tick: drain inbound messages, call raft_periodic().
 * Call this every ORACLE_TICK_MS from the raft task loop.
 *
 * @param ctx       This node's context
 * @param elapsed_ms Milliseconds since last call
 * @return 0 on success
 */
int oracle_tick(oracle_node_ctx_t *ctx, int elapsed_ms);

/**
 * Propose a health state transition to the Raft cluster.
 * Only the leader can successfully propose; followers return error.
 *
 * @param ctx           This node's context
 * @param target_node   Node whose status changed
 * @param target_box    Box of the target node
 * @param old_status    Previous status
 * @param new_status    New status
 * @return 0 on success, RAFT_ERR_NOT_LEADER if not leader
 */
int oracle_propose_health_transition(oracle_node_ctx_t *ctx,
                                     uint8_t target_node, uint8_t target_box,
                                     uint8_t old_status, uint8_t new_status);

/**
 * Check if this node is the Raft leader.
 */
int oracle_is_leader(oracle_node_ctx_t *ctx);

/**
 * Dispatch an incoming Raft message (from transport receive loop).
 *
 * @param ctx           This node's context
 * @param src_node_id   Sender's node ID
 * @param msg_type      Message type from wire header
 * @param term          Raft term from wire frame header
 * @param payload       Deserialized payload bytes
 * @param len           Payload length
 * @return 0 on success
 */
int oracle_dispatch_raft_message(oracle_node_ctx_t *ctx,
                                 uint8_t src_node_id,
                                 raft_msg_type_t msg_type,
                                 uint32_t term,
                                 const void *payload, size_t len);

/**
 * Free oracle resources.
 */
void oracle_destroy(oracle_node_ctx_t *ctx);

#endif /* RAFT_ORACLE_H */

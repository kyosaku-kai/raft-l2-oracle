#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

/**
 * node_config.h - Compile-time node identity and cluster membership
 *
 * Each STM32 board is flashed with a unique NODE_ID via:
 *   cmake -DNODE_ID=2 ..
 *
 * The peer table defines all cluster members. For the hackathon demo
 * all 3 STM32s share box_id=1 (single-box). In production each box
 * has one STM32 and box_id differs per chassis.
 */

#include <stdint.h>

/* --- Per-board identity (set via cmake -DNODE_ID=N) --- */

#ifndef ORACLE_THIS_NODE_ID
#define ORACLE_THIS_NODE_ID 1
#endif

#ifndef ORACLE_THIS_BOX_ID
#define ORACLE_THIS_BOX_ID 1
#endif

/* --- Cluster peer table --- */

typedef struct {
    uint8_t node_id;
    uint8_t box_id;
    uint8_t mac[6];
} oracle_peer_t;

/*
 * MAC scheme: 02:CA:FE:<box_id>:00:<node_id>
 *
 * For hackathon (single-box, 3 STM32s on same switch):
 *   Node 1: 02:CA:FE:01:00:01
 *   Node 2: 02:CA:FE:01:00:02
 *   Node 3: 02:CA:FE:01:00:03
 *
 * Compute node IDs start at 101 (matching oracle-agent ORACLE_NODE_ID).
 */

/*
 * Cluster size: override with cmake -DCLUSTER_NODES=1 for single-board testing.
 * Default: 3 (full cluster). With CLUSTER_NODES=1, only self is added to raft —
 * single-node quorum means immediate leader election without peers.
 */
#ifndef ORACLE_NUM_CLUSTER_NODES
#define ORACLE_NUM_CLUSTER_NODES 3
#endif

static const oracle_peer_t oracle_cluster_peers[] = {
    { .node_id = 1, .box_id = 1, .mac = { 0x02, 0xCA, 0xFE, 0x01, 0x00, 0x01 } },
    { .node_id = 2, .box_id = 1, .mac = { 0x02, 0xCA, 0xFE, 0x01, 0x00, 0x02 } },
    { .node_id = 3, .box_id = 1, .mac = { 0x02, 0xCA, 0xFE, 0x01, 0x00, 0x03 } },
};

/** Find this node's entry in the peer table. Returns NULL if not found. */
static inline const oracle_peer_t *oracle_get_self(void)
{
    for (int i = 0; i < ORACLE_NUM_CLUSTER_NODES; i++) {
        if (oracle_cluster_peers[i].node_id == ORACLE_THIS_NODE_ID)
            return &oracle_cluster_peers[i];
    }
    return (const oracle_peer_t *)0;
}

#endif /* NODE_CONFIG_H */

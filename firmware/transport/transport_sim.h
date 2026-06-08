#ifndef TRANSPORT_SIM_H
#define TRANSPORT_SIM_H

/**
 * transport_sim.h - POSIX UDP loopback transport for the simulator
 *
 * Each simulated node gets a UDP socket on 127.0.0.1:<base_port + node_id>.
 * Messages are serialized into the wire format header + payload, sent as
 * UDP datagrams. No actual Ethernet framing - just the oracle header + payload.
 */

#include "transport.h"
#include <stdint.h>

#define SIM_BASE_PORT 5000

/**
 * Create a simulator transport instance.
 *
 * @param node_id   This node's ID (determines listen port)
 * @param box_id    This node's box ID
 * @return Transport instance (caller must free with sim_transport_destroy)
 */
raft_transport_t *sim_transport_create(uint8_t node_id, uint8_t box_id);

/**
 * Destroy a simulator transport instance.
 */
void sim_transport_destroy(raft_transport_t *t);

/**
 * Register all node IDs for broadcast support (dst_node=0xFF).
 * Call once after all transports are created.
 *
 * @param node_ids  Array of node IDs in the cluster
 * @param n_nodes   Number of nodes
 */
void sim_transport_set_broadcast_peers(const uint8_t *node_ids, int n_nodes);

#endif /* TRANSPORT_SIM_H */

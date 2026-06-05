#ifndef TRANSPORT_H
#define TRANSPORT_H

/**
 * transport.h - Transport HAL for the L2 Oracle
 *
 * Abstracts the network layer so the same Raft integration code works
 * on STM32 (raw Ethernet MAC), POSIX simulator (UDP loopback), and
 * eventually production SoC service processors (vendor SDK inject/extract).
 */

#include "../protocol/wire_format.h"
#include <stdint.h>
#include <stddef.h>

typedef struct raft_transport raft_transport_t;

struct raft_transport {
    /**
     * Send a message to a specific node.
     *
     * @param t         Transport instance
     * @param dst_node  Destination node ID
     * @param ethertype EtherType (0x88B5, 0x88B6, 0x88B7)
     * @param type      Message type (raft_msg_type_t)
     * @param payload   Payload bytes (already serialized)
     * @param len       Payload length
     * @return 0 on success, -1 on error
     */
    int (*send)(raft_transport_t *t, uint8_t dst_node, uint16_t ethertype,
                raft_msg_type_t type, const void *payload, size_t len);

    /**
     * Receive a message (blocking with timeout).
     *
     * @param t         Transport instance
     * @param ethertype [out] EtherType of received frame
     * @param type      [out] Message type
     * @param src_node  [out] Sender's node ID
     * @param payload   [out] Payload buffer
     * @param max_len   Max payload buffer size
     * @param timeout_ms Timeout (0 = non-blocking poll)
     * @return Bytes received, 0 on timeout, -1 on error
     */
    int (*recv)(raft_transport_t *t, uint16_t *ethertype,
                raft_msg_type_t *type, uint8_t *src_node,
                void *payload, size_t max_len, uint32_t timeout_ms);

    /**
     * Get current monotonic time in milliseconds.
     */
    uint64_t (*now_ms)(raft_transport_t *t);

    /** Opaque transport-specific data */
    void *impl_data;
};

#endif /* TRANSPORT_H */

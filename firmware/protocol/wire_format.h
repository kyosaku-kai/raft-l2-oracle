#ifndef WIRE_FORMAT_H
#define WIRE_FORMAT_H

/**
 * wire_format.h - L2 Oracle Wire Protocol (v1)
 *
 * All messages are raw Ethernet II frames with custom EtherTypes.
 * Structs are packed, little-endian (ARM native). EtherType field
 * follows Ethernet II convention (big-endian on wire).
 *
 * See: docs/design.md Section 4
 */

#include <stdint.h>
#include <string.h>

/* --- EtherTypes --- */

#define ETHERTYPE_RAFT      0x88B5  /* STM32 <-> STM32 (Raft consensus) */
#define ETHERTYPE_HEALTH    0x88B6  /* STM32 -> local compute (health reports) */
#define ETHERTYPE_HEARTBEAT 0x88B7  /* Compute -> local STM32 (heartbeats) */

/* --- MAC Address Scheme --- */
/* Locally administered: 02:CA:FE:<box>:00:<role>
 * role 01 = STM32, other roles reserved */

#define MAC_PREFIX_0 0x02
#define MAC_PREFIX_1 0xCA
#define MAC_PREFIX_2 0xFE

/* Build a STM32 MAC address: 02:CA:FE:<box_id>:00:01 */
#define ORACLE_MAC_STM32(mac, box_id) do { \
    (mac)[0] = MAC_PREFIX_0; \
    (mac)[1] = MAC_PREFIX_1; \
    (mac)[2] = MAC_PREFIX_2; \
    (mac)[3] = (box_id);    \
    (mac)[4] = 0x00;        \
    (mac)[5] = 0x01;        \
} while (0)

/* --- Message Types --- */

typedef enum {
    /* Raft consensus (EtherType 0x88B5) - STM32 <-> STM32 inter-box */
    MSG_REQUEST_VOTE          = 0x01,
    MSG_REQUEST_VOTE_RESP     = 0x02,
    MSG_APPEND_ENTRIES        = 0x03,
    MSG_APPEND_ENTRIES_RESP   = 0x04,

    /* Health reports (EtherType 0x88B6) - STM32 -> local compute */
    MSG_HEALTH_UPDATE         = 0x10,
    MSG_CLUSTER_STATE         = 0x11,
    MSG_FAILURE_EVENT         = 0x12,

    /* Node heartbeats (EtherType 0x88B7) - compute -> local STM32 */
    MSG_NODE_HEARTBEAT        = 0x20,
    MSG_NODE_ANNOUNCE         = 0x21,
    MSG_NODE_ANNOUNCE_ACK     = 0x22,
} raft_msg_type_t;

/* --- Frame Header (24 bytes) --- */

typedef struct __attribute__((packed)) {
    /* Ethernet II header (14 bytes) */
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;         /* 0x88B5, 0x88B6, or 0x88B7 (big-endian on wire) */

    /* Oracle protocol header (10 bytes) */
    uint8_t  version;           /* upper nibble = version (1), lower = reserved */
    uint8_t  msg_type;          /* raft_msg_type_t */
    uint8_t  node_id;           /* sender's node ID */
    uint8_t  box_id;            /* sender's box ID */
    uint32_t term;              /* Raft term (little-endian). 0 for non-Raft. */
    uint16_t payload_len;       /* little-endian. Max 64 for v1. */
} oracle_frame_header_t;

_Static_assert(sizeof(oracle_frame_header_t) == 24, "header must be 24 bytes");

#define ORACLE_PROTOCOL_VERSION 0x10  /* upper nibble = 1, lower = reserved(0) */
#define ORACLE_MAX_PAYLOAD_V1   64

/* --- Raft Payload Structs (EtherType 0x88B5) --- */

/** MSG_REQUEST_VOTE (0x01) - 16 bytes */
typedef struct __attribute__((packed)) {
    uint32_t candidate_id;
    uint32_t last_log_idx;
    uint32_t last_log_term;
    uint32_t _reserved;
} payload_request_vote_t;

_Static_assert(sizeof(payload_request_vote_t) == 16, "request_vote payload must be 16 bytes");

/** MSG_REQUEST_VOTE_RESP (0x02) - 8 bytes */
typedef struct __attribute__((packed)) {
    uint32_t vote_granted;      /* 1 = granted, 0 = denied */
    uint32_t current_idx;       /* responder's current log index */
} payload_request_vote_resp_t;

_Static_assert(sizeof(payload_request_vote_resp_t) == 8, "request_vote_resp payload must be 8 bytes");

/** Wire format for a single log entry within AppendEntries */
typedef struct __attribute__((packed)) {
    uint32_t term;
    uint32_t index;
    uint8_t  entry_type;        /* HEALTH_TRANSITION or MEMBERSHIP */
    uint8_t  target_node_id;
    uint8_t  target_box_id;
    uint8_t  old_status;        /* NODE_UP=0, NODE_SUSPECT=1, NODE_DOWN=2 */
    uint8_t  new_status;
    uint8_t  _pad[3];
    uint32_t timestamp_ms;
    uint64_t _reserved;
} raft_log_entry_wire_t;

_Static_assert(sizeof(raft_log_entry_wire_t) == 28, "log entry wire must be 28 bytes");

/** MSG_APPEND_ENTRIES (0x03) - 16 + N*32 bytes */
typedef struct __attribute__((packed)) {
    uint32_t prev_log_idx;
    uint32_t prev_log_term;
    uint32_t leader_commit;
    uint8_t  n_entries;         /* 0 = heartbeat */
    uint8_t  _pad[3];
    /* followed by n_entries * raft_log_entry_wire_t */
} payload_append_entries_t;

_Static_assert(sizeof(payload_append_entries_t) == 16, "append_entries header must be 16 bytes");

/** Follower observation sideband - piggybacked on AE response */
typedef struct __attribute__((packed)) {
    uint8_t  node_id;
    uint8_t  observed_status;   /* NODE_UP=0, NODE_SUSPECT=1, NODE_DOWN=2 */
    uint8_t  confidence;        /* heartbeats_received / heartbeats_expected (0-255) */
    uint8_t  _pad;
} observation_entry_t;

_Static_assert(sizeof(observation_entry_t) == 4, "observation entry must be 4 bytes");

#define MAX_OBSERVATIONS_PER_RESP 8

/** MSG_APPEND_ENTRIES_RESP (0x04) - up to 48 bytes */
typedef struct __attribute__((packed)) {
    /* Standard Raft fields */
    uint32_t success;           /* 1 = success, 0 = failure */
    uint32_t current_idx;
    uint32_t first_idx;

    /* Follower observation sideband */
    uint8_t  n_observations;
    uint8_t  _obs_pad[3];
    observation_entry_t obs[MAX_OBSERVATIONS_PER_RESP];
} payload_append_entries_resp_t;

_Static_assert(sizeof(payload_append_entries_resp_t) == 48, "append_entries_resp must be 48 bytes");

/* --- Health Payload Structs (EtherType 0x88B6) --- */

/** MSG_HEALTH_UPDATE (0x10) and MSG_FAILURE_EVENT (0x12) - 12 bytes */
typedef struct __attribute__((packed)) {
    uint8_t  target_node_id;
    uint8_t  target_box_id;
    uint8_t  old_status;
    uint8_t  new_status;
    uint32_t term;
    uint32_t timestamp_ms;
} payload_health_update_t;

_Static_assert(sizeof(payload_health_update_t) == 12, "health_update must be 12 bytes");

/** MSG_CLUSTER_STATE (0x11) - 4 + N*12 bytes */
typedef struct __attribute__((packed)) {
    uint8_t  n_nodes;
    uint8_t  leader_node_id;
    uint8_t  leader_box_id;
    uint8_t  quorum_healthy;    /* 1 = oracle has quorum */
    /* followed by n_nodes * node_health_wire_t */
} payload_cluster_state_t;

_Static_assert(sizeof(payload_cluster_state_t) == 4, "cluster_state header must be 4 bytes");

/** Per-node entry in cluster state dump */
typedef struct __attribute__((packed)) {
    uint8_t  node_id;
    uint8_t  box_id;
    uint8_t  node_type;
    uint8_t  status;
    uint32_t last_seen_ms;
    uint32_t status_term;
} node_health_wire_t;

_Static_assert(sizeof(node_health_wire_t) == 12, "node_health_wire must be 12 bytes");

/* --- Heartbeat Payload Structs (EtherType 0x88B7) --- */

/** MSG_NODE_HEARTBEAT (0x20) - 8 bytes */
typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint16_t load_pct;          /* CPU load 0-1000 (0.1% resolution) */
    uint16_t _reserved;
} payload_node_heartbeat_t;

_Static_assert(sizeof(payload_node_heartbeat_t) == 8, "node_heartbeat must be 8 bytes");

/** MSG_NODE_ANNOUNCE (0x21) - 16 bytes */
typedef struct __attribute__((packed)) {
    uint8_t  node_type;         /* NODE_TYPE_JETSON=1, NODE_TYPE_X86=2 */
    uint8_t  _pad[3];
    uint8_t  mac[6];            /* node's real MAC */
    uint8_t  hostname[6];       /* first 6 chars of hostname (debug aid) */
} payload_node_announce_t;

_Static_assert(sizeof(payload_node_announce_t) == 16, "node_announce must be 16 bytes");

/** MSG_NODE_ANNOUNCE_ACK (0x22) - 4 bytes */
typedef struct __attribute__((packed)) {
    uint8_t  assigned_node_id;
    uint8_t  box_id;
    uint8_t  status;            /* initial status (NODE_UP) */
    uint8_t  _pad;
} payload_node_announce_ack_t;

_Static_assert(sizeof(payload_node_announce_ack_t) == 4, "node_announce_ack must be 4 bytes");

/* --- Helper: build frame header --- */

static inline void oracle_frame_init(oracle_frame_header_t *hdr,
                                     const uint8_t *dst_mac,
                                     const uint8_t *src_mac,
                                     uint16_t ethertype,
                                     uint8_t msg_type,
                                     uint8_t node_id,
                                     uint8_t box_id,
                                     uint32_t term,
                                     uint16_t payload_len)
{
    memcpy(hdr->dst_mac, dst_mac, 6);
    memcpy(hdr->src_mac, src_mac, 6);
    /* EtherType is big-endian on wire */
    hdr->ethertype = __builtin_bswap16(ethertype);
    hdr->version = ORACLE_PROTOCOL_VERSION;
    hdr->msg_type = msg_type;
    hdr->node_id = node_id;
    hdr->box_id = box_id;
    hdr->term = term;           /* little-endian (native) */
    hdr->payload_len = payload_len;
}

#endif /* WIRE_FORMAT_H */

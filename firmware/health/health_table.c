/**
 * health_table.c - Replicated health state table
 *
 * The health table is the authoritative record of node health states,
 * updated when Raft commits health transition entries via cb_applylog().
 * On state changes, emits 0x88B6 health events to the local compute node.
 * Also provides periodic MSG_CLUSTER_STATE broadcast for oracle-agent.
 */

#include "health_monitor.h"
#include "../raft/raft_oracle.h"

#include <string.h>
#include <stdio.h>

void health_table_apply(health_monitor_t *hm,
                        uint8_t target_node, uint8_t target_box,
                        uint8_t old_status, uint8_t new_status,
                        uint32_t term)
{
    /* Find or create entry */
    int idx = -1;
    for (int i = 0; i < hm->n_nodes; i++) {
        if (hm->table[i].node_id == target_node) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        /* Node not in local table yet - create entry */
        if (hm->n_nodes >= MAX_HEALTH_NODES)
            return;
        idx = hm->n_nodes++;
        memset(&hm->table[idx], 0, sizeof(hm->table[idx]));
        hm->table[idx].node_id = target_node;
        hm->table[idx].box_id = target_box;
    }

    node_health_entry_t *e = &hm->table[idx];
    uint8_t prev = e->status;
    e->status = new_status;
    e->status_term = term;

    printf("health_table: node %d committed %d -> %d (term %lu)\r\n",
           target_node, old_status, new_status, (unsigned long)term);

    /* Emit 0x88B6 health event to local compute node on state change */
    if (prev != new_status && hm->transport) {
        payload_health_update_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.target_node_id = target_node;
        evt.target_box_id = target_box;
        evt.old_status = old_status;
        evt.new_status = new_status;
        evt.term = term;
        evt.timestamp_ms = e->last_seen_ms;

        raft_msg_type_t msg_type = (new_status == NODE_DOWN)
            ? MSG_FAILURE_EVENT : MSG_HEALTH_UPDATE;

        /* Broadcast to all local compute nodes (use node 0xFF as broadcast) */
        hm->transport->send(hm->transport, 0xFF,
                            ETHERTYPE_HEALTH, msg_type, term,
                            &evt, sizeof(evt));
    }
}

void health_table_broadcast_cluster_state(health_monitor_t *hm,
                                          oracle_node_ctx_t *ctx)
{
    if (!hm->transport || !ctx->raft)
        return;

    /* Only the leader broadcasts cluster state */
    if (!oracle_is_leader(ctx))
        return;

    /*
     * Build MSG_CLUSTER_STATE frame: 4-byte header + N * 12-byte entries.
     * Must fit in ORACLE_MAX_PAYLOAD_V1 (64 bytes) → max 5 nodes.
     */
    uint8_t buf[sizeof(payload_cluster_state_t) +
                MAX_HEALTH_NODES * sizeof(node_health_wire_t)];

    payload_cluster_state_t *cs = (payload_cluster_state_t *)buf;
    cs->n_nodes = hm->n_nodes;
    cs->leader_node_id = ctx->node_id;
    cs->leader_box_id = ctx->box_id;

    /* Quorum: majority of cluster nodes are reachable via raft */
    int n_raft_nodes = raft_get_num_nodes(ctx->raft);
    int n_voting = 0;
    for (int i = 0; i < n_raft_nodes; i++) {
        raft_node_t *rn = raft_get_node_from_idx(ctx->raft, i);
        if (rn && raft_node_is_voting(rn))
            n_voting++;
    }
    cs->quorum_healthy = (n_voting > 0) ? 1 : 0;

    /* Fill per-node health entries */
    node_health_wire_t *entries =
        (node_health_wire_t *)(buf + sizeof(payload_cluster_state_t));
    uint8_t n = hm->n_nodes;
    if (n > MAX_HEALTH_NODES)
        n = MAX_HEALTH_NODES;
    /* Clamp to what fits in max payload */
    uint8_t max_entries = (ORACLE_MAX_PAYLOAD_V1 - sizeof(payload_cluster_state_t))
                          / sizeof(node_health_wire_t);
    if (n > max_entries)
        n = max_entries;
    cs->n_nodes = n;

    for (uint8_t i = 0; i < n; i++) {
        entries[i].node_id      = hm->table[i].node_id;
        entries[i].box_id       = hm->table[i].box_id;
        entries[i].node_type    = hm->table[i].node_type;
        entries[i].status       = hm->table[i].status;
        entries[i].last_seen_ms = hm->table[i].last_seen_ms;
        entries[i].status_term  = hm->table[i].status_term;
    }

    size_t total = sizeof(payload_cluster_state_t) + n * sizeof(node_health_wire_t);
    uint32_t term = (uint32_t)raft_get_current_term(ctx->raft);

    hm->transport->send(hm->transport, 0xFF,
                        ETHERTYPE_HEALTH, MSG_CLUSTER_STATE, term,
                        buf, total);
}

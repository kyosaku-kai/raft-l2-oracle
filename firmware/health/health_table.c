/**
 * health_table.c - Replicated health state table
 *
 * The health table is the authoritative record of node health states,
 * updated when Raft commits health transition entries via cb_applylog().
 * On state changes, emits 0x88B6 health events to the local compute node.
 */

#include "health_monitor.h"

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

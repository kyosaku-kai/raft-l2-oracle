/**
 * health_monitor.c - Health monitoring state machine
 *
 * Tracks heartbeats from local compute nodes, detects failures via
 * UP->SUSPECT->DOWN state transitions, proposes transitions via Raft,
 * and maintains leader-side corroboration table.
 */

#include "health_monitor.h"
#include "../raft/raft_oracle.h"

#include <string.h>
#include <stdio.h>

/* Verify ORACLE_MAX_NODES matches between headers */
_Static_assert(ORACLE_MAX_NODES == 8,
               "ORACLE_MAX_NODES mismatch - update health_monitor.h");

/* --- Internal helpers --- */

static int find_node(const health_monitor_t *hm, uint8_t node_id)
{
    for (int i = 0; i < hm->n_nodes; i++) {
        if (hm->table[i].node_id == node_id)
            return i;
    }
    return -1;
}

static int find_or_alloc(health_monitor_t *hm, uint8_t node_id)
{
    int idx = find_node(hm, node_id);
    if (idx >= 0)
        return idx;
    if (hm->n_nodes >= MAX_HEALTH_NODES)
        return -1;
    idx = hm->n_nodes++;
    memset(&hm->table[idx], 0, sizeof(hm->table[idx]));
    hm->table[idx].node_id = node_id;
    hm->miss_count[idx] = 0;
    hm->suspect_since_ms[idx] = 0;
    return idx;
}

static void propose_transition(health_monitor_t *hm, int idx,
                               uint8_t new_status)
{
    node_health_entry_t *e = &hm->table[idx];
    uint8_t old_status = e->status;

    if (old_status == new_status)
        return;

    printf("health: node %d transition %d -> %d\r\n",
           e->node_id, old_status, new_status);

    oracle_propose_health_transition(
        (oracle_node_ctx_t *)hm->oracle,
        e->node_id, e->box_id,
        old_status, new_status);

    /* Update local status immediately for observation reporting.
     * The authoritative update comes via health_table_apply() when
     * the Raft entry commits. For single-node clusters this is
     * effectively the same; for multi-node, the local view may
     * briefly lead the committed view. */
    e->status = new_status;
}

static int is_leader(const health_monitor_t *hm)
{
    return oracle_is_leader((oracle_node_ctx_t *)hm->oracle);
}

/**
 * Check if DOWN transition for target_node is corroborated by K peers.
 * For intra-box nodes (same box_id as this STM32), skip corroboration
 * since only one STM32 can observe them.
 */
static int corroborated(const health_monitor_t *hm,
                        uint8_t target_node, uint8_t target_box,
                        uint32_t now_ms)
{
    /* Intra-box: this STM32 is the only observer, proceed without corroboration */
    if (target_box == hm->box_id) {
        (void)target_node;
        return 1;
    }

    uint8_t agree = 0;
    for (int p = 0; p < ORACLE_MAX_NODES; p++) {
        if (now_ms - hm->corroboration.updated_ms[p] > STALE_OBS_MS)
            continue;
        if (hm->corroboration.status[p][target_node] >= NODE_SUSPECT)
            agree++;
    }
    return agree >= CORROBORATION_K;
}

/* --- Public API --- */

void health_monitor_init(health_monitor_t *hm,
                         struct oracle_node_ctx *oracle,
                         raft_transport_t *transport,
                         uint8_t box_id)
{
    memset(hm, 0, sizeof(*hm));
    hm->oracle = oracle;
    hm->transport = transport;
    hm->box_id = box_id;
}

void health_monitor_process_heartbeat(health_monitor_t *hm,
                                      uint8_t src_node,
                                      const payload_node_heartbeat_t *hb,
                                      uint32_t now_ms)
{
    (void)hb; /* seq and load_pct available for future use */

    int idx = find_node(hm, src_node);
    if (idx < 0)
        return; /* not registered - ignore until we get an announce */

    node_health_entry_t *e = &hm->table[idx];
    e->last_seen_ms = now_ms;
    hm->miss_count[idx] = 0;

    /* Recovery: SUSPECT or DOWN -> UP */
    if (e->status == NODE_SUSPECT || e->status == NODE_DOWN) {
        propose_transition(hm, idx, NODE_UP);
        hm->suspect_since_ms[idx] = 0;
    }
}

void health_monitor_process_announce(health_monitor_t *hm,
                                     uint8_t src_node,
                                     const payload_node_announce_t *ann,
                                     uint32_t now_ms)
{
    int idx = find_or_alloc(hm, src_node);
    if (idx < 0) {
        printf("health: table full, cannot register node %d\r\n", src_node);
        return;
    }

    node_health_entry_t *e = &hm->table[idx];
    e->box_id = hm->box_id; /* local compute nodes share our box_id */
    e->node_type = ann->node_type;
    e->status = NODE_UP;
    e->last_seen_ms = now_ms;
    e->status_term = 0;
    hm->miss_count[idx] = 0;
    hm->suspect_since_ms[idx] = 0;

    printf("health: registered node %d type=%d\r\n", src_node, ann->node_type);

    /* Send ACK back */
    payload_node_announce_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.assigned_node_id = src_node;
    ack.box_id = hm->box_id;
    ack.status = NODE_UP;

    if (hm->transport) {
        hm->transport->send(hm->transport, src_node,
                            ETHERTYPE_HEARTBEAT, MSG_NODE_ANNOUNCE_ACK, 0,
                            &ack, sizeof(ack));
    }
}

void health_monitor_tick(health_monitor_t *hm, uint32_t now_ms)
{
    /* Skip if no time has passed or first call */
    if (hm->last_tick_ms == 0) {
        hm->last_tick_ms = now_ms;
        return;
    }

    uint32_t elapsed = now_ms - hm->last_tick_ms;
    hm->last_tick_ms = now_ms;

    if (elapsed == 0)
        return;

    for (int i = 0; i < hm->n_nodes; i++) {
        node_health_entry_t *e = &hm->table[i];

        /* How many heartbeat intervals have elapsed since last seen? */
        uint32_t since_last = now_ms - e->last_seen_ms;

        if (since_last >= HB_INTERVAL_MS) {
            /* Count misses based on time elapsed, not tick count */
            hm->miss_count[i] = (uint16_t)(since_last / HB_INTERVAL_MS);
        }

        switch (e->status) {
        case NODE_UP:
            if (hm->miss_count[i] >= SUSPECT_THRESHOLD) {
                hm->suspect_since_ms[i] = now_ms;
                propose_transition(hm, i, NODE_SUSPECT);
            }
            break;

        case NODE_SUSPECT:
            if (now_ms - hm->suspect_since_ms[i] >= DOWN_TIMEOUT_MS) {
                if (is_leader(hm)) {
                    if (corroborated(hm, e->node_id, e->box_id, now_ms)) {
                        propose_transition(hm, i, NODE_DOWN);
                    }
                } else {
                    /* Followers just update local status for observation
                     * reporting. The leader will propose via Raft. */
                    e->status = NODE_DOWN;
                    printf("health: node %d locally marked DOWN\r\n",
                           e->node_id);
                }
            }
            break;

        case NODE_DOWN:
            /* Waiting for heartbeat recovery (handled in process_heartbeat) */
            break;
        }
    }
}

void health_monitor_get_observations(health_monitor_t *hm,
                                     observation_entry_t *out,
                                     uint8_t *n_out)
{
    uint8_t count = 0;
    uint8_t max = MAX_OBSERVATIONS_PER_RESP;

    for (int i = 0; i < hm->n_nodes && count < max; i++) {
        node_health_entry_t *e = &hm->table[i];
        out[count].node_id = e->node_id;
        out[count].observed_status = e->status;

        /* Confidence: 255=UP, 128=SUSPECT, 0=DOWN */
        switch (e->status) {
        case NODE_UP:      out[count].confidence = 255; break;
        case NODE_SUSPECT: out[count].confidence = 128; break;
        case NODE_DOWN:    out[count].confidence = 0;   break;
        default:           out[count].confidence = 0;   break;
        }
        out[count]._pad = 0;
        count++;
    }
    *n_out = count;
}

void health_monitor_update_corroboration(health_monitor_t *hm,
                                         uint8_t peer_id,
                                         const observation_entry_t *obs,
                                         uint8_t n_obs,
                                         uint32_t now_ms)
{
    if (peer_id >= ORACLE_MAX_NODES)
        return;

    for (uint8_t i = 0; i < n_obs; i++) {
        uint8_t nid = obs[i].node_id;
        if (nid < MAX_HEALTH_NODES) {
            hm->corroboration.status[peer_id][nid] = obs[i].observed_status;
        }
    }
    hm->corroboration.updated_ms[peer_id] = now_ms;
}

const node_health_entry_t *health_monitor_get_table(const health_monitor_t *hm,
                                                    uint8_t *n_nodes)
{
    *n_nodes = hm->n_nodes;
    return hm->table;
}

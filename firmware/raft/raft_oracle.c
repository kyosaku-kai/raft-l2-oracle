/**
 * raft_oracle.c - Raft oracle integration with willemt/raft
 *
 * Implements the callback bridge between willemt/raft and our
 * transport HAL + wire format. This file is shared between the
 * STM32 firmware and the POSIX simulator.
 */

#include "raft_oracle.h"
#include "../protocol/wire_format.h"
#include "../transport/transport.h"
#include "../health/health_monitor.h"

#include <string.h>
#include <stdio.h>

#include "raft_private.h"  /* __raft_malloc, __raft_free */

/* --- Raft Callback Implementations --- */

static int cb_send_requestvote(raft_server_t *raft, void *udata,
                               raft_node_t *node, msg_requestvote_t *msg)
{
    (void)raft;
    oracle_node_ctx_t *ctx = (oracle_node_ctx_t *)udata;
    raft_node_id_t peer_id = raft_node_get_id(node);

    payload_request_vote_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.candidate_id = (uint32_t)msg->candidate_id;
    payload.last_log_idx = (uint32_t)msg->last_log_idx;
    payload.last_log_term = (uint32_t)msg->last_log_term;

    return ctx->transport->send(ctx->transport, (uint8_t)peer_id,
                                ETHERTYPE_RAFT, MSG_REQUEST_VOTE,
                                &payload, sizeof(payload));
}

static int cb_send_appendentries(raft_server_t *raft, void *udata,
                                 raft_node_t *node, msg_appendentries_t *msg)
{
    (void)raft;
    oracle_node_ctx_t *ctx = (oracle_node_ctx_t *)udata;
    raft_node_id_t peer_id = raft_node_get_id(node);

    /*
     * Build wire-format payload: fixed header + N entry records.
     * For v1, we cap at 1 entry per message to keep within 64-byte payload.
     * Heartbeats (n_entries=0) are just the 16-byte header.
     */
    uint8_t buf[sizeof(payload_append_entries_t) +
                sizeof(raft_log_entry_wire_t)];

    payload_append_entries_t *ae = (payload_append_entries_t *)buf;
    memset(ae, 0, sizeof(*ae));
    ae->prev_log_idx = (uint32_t)msg->prev_log_idx;
    ae->prev_log_term = (uint32_t)msg->prev_log_term;
    ae->leader_commit = (uint32_t)msg->leader_commit;
    ae->n_entries = (uint8_t)(msg->n_entries > 1 ? 1 : msg->n_entries);

    size_t total_len = sizeof(payload_append_entries_t);

    if (ae->n_entries > 0 && msg->entries) {
        raft_log_entry_wire_t *we = (raft_log_entry_wire_t *)(buf + sizeof(*ae));
        memset(we, 0, sizeof(*we));
        we->term = (uint32_t)msg->entries[0].term;
        we->index = 0; /* receiver computes from prev_log_idx */
        we->entry_type = (uint8_t)msg->entries[0].type;

        /* If entry has data, try to extract health transition fields */
        if (msg->entries[0].data.buf && msg->entries[0].data.len > 0) {
            /* For now, just copy raw bytes up to what fits */
            size_t copy_len = msg->entries[0].data.len;
            if (copy_len > sizeof(raft_log_entry_wire_t) - 8)
                copy_len = sizeof(raft_log_entry_wire_t) - 8;
            memcpy(&we->target_node_id, msg->entries[0].data.buf, copy_len);
        }

        total_len += sizeof(raft_log_entry_wire_t);
    }

    return ctx->transport->send(ctx->transport, (uint8_t)peer_id,
                                ETHERTYPE_RAFT, MSG_APPEND_ENTRIES,
                                buf, total_len);
}

static int cb_persist_term(raft_server_t *raft, void *udata,
                           raft_term_t term, raft_node_id_t vote)
{
    (void)raft;
    oracle_node_ctx_t *ctx = (oracle_node_ctx_t *)udata;
    ctx->persisted_term = term;
    ctx->persisted_vote = vote;
    return 0;
}

static int cb_persist_vote(raft_server_t *raft, void *udata,
                           raft_node_id_t vote)
{
    (void)raft;
    oracle_node_ctx_t *ctx = (oracle_node_ctx_t *)udata;
    ctx->persisted_vote = vote;
    return 0;
}

static int cb_applylog(raft_server_t *raft, void *udata,
                       raft_entry_t *entry, raft_index_t idx)
{
    (void)raft;
    (void)idx;
    oracle_node_ctx_t *ctx = (oracle_node_ctx_t *)udata;

    if (entry->type == RAFT_LOGTYPE_NORMAL && entry->data.buf &&
        entry->data.len >= sizeof(payload_health_update_t))
    {
        const payload_health_update_t *ht =
            (const payload_health_update_t *)entry->data.buf;

        if (ctx->health_ctx) {
            health_table_apply((health_monitor_t *)ctx->health_ctx,
                               ht->target_node_id, ht->target_box_id,
                               ht->old_status, ht->new_status,
                               ht->term);
        }
    }
    return 0;
}

static int cb_log_offer(raft_server_t *raft, void *udata,
                        raft_entry_t *entry, raft_index_t idx)
{
    (void)raft;
    (void)udata;
    (void)idx;

    /* Deep-copy entry data so it persists after the message buffer is freed.
     * Uses the pluggable allocator so bare-metal routes through pvPortMalloc. */
    if (entry->data.buf && entry->data.len > 0) {
        void *copy = __raft_malloc(entry->data.len);
        if (!copy) return -1;
        memcpy(copy, entry->data.buf, entry->data.len);
        entry->data.buf = copy;
    }
    return 0;
}

static int cb_log_pop(raft_server_t *raft, void *udata,
                      raft_entry_t *entry, raft_index_t idx)
{
    (void)raft;
    (void)udata;
    (void)idx;
    if (entry->data.buf) {
        __raft_free(entry->data.buf);
        entry->data.buf = NULL;
    }
    return 0;
}

static int cb_log_poll(raft_server_t *raft, void *udata,
                       raft_entry_t *entry, raft_index_t idx)
{
    return cb_log_pop(raft, udata, entry, idx);
}

static int cb_log_clear(raft_server_t *raft, void *udata,
                        raft_entry_t *entry, raft_index_t idx)
{
    return cb_log_pop(raft, udata, entry, idx);
}

static int cb_log_get_node_id(raft_server_t *raft, void *udata,
                              raft_entry_t *entry, raft_index_t idx)
{
    (void)raft;
    (void)udata;
    (void)idx;
    if (entry->data.buf && entry->data.len >= sizeof(uint32_t))
        return *(int *)entry->data.buf;
    return -1;
}

/* Enable ORACLE_DEBUG_LOG to get verbose raft library output on stderr */
#ifdef ORACLE_DEBUG_LOG
static void cb_log(raft_server_t *raft, raft_node_t *node, void *udata,
                   const char *buf)
{
    oracle_node_ctx_t *ctx = (oracle_node_ctx_t *)udata;
    (void)raft;
    (void)node;
    fprintf(stderr, "[node %d] %s\n", ctx->node_id, buf);
}
#endif

/* --- Public API --- */

int oracle_init(oracle_node_ctx_t *ctx, uint8_t node_id, uint8_t box_id,
                raft_transport_t *transport)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->node_id = node_id;
    ctx->box_id = box_id;
    ctx->transport = transport;
    ctx->next_entry_id = 1;

    ctx->raft = raft_new();
    if (!ctx->raft)
        return -1;

    raft_cbs_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.send_requestvote = cb_send_requestvote;
    cbs.send_appendentries = cb_send_appendentries;
    cbs.persist_term = cb_persist_term;
    cbs.persist_vote = cb_persist_vote;
    cbs.applylog = cb_applylog;
    cbs.log_offer = cb_log_offer;
    cbs.log_poll = cb_log_poll;
    cbs.log_pop = cb_log_pop;
    cbs.log_clear = cb_log_clear;
    cbs.log_get_node_id = cb_log_get_node_id;
#ifdef ORACLE_DEBUG_LOG
    cbs.log = cb_log;
#endif

    raft_set_callbacks(ctx->raft, &cbs, ctx);

    return 0;
}

int oracle_add_node(oracle_node_ctx_t *ctx, uint8_t peer_id, int is_self)
{
    raft_node_t *node = raft_add_node(ctx->raft, NULL, peer_id, is_self);
    return node ? 0 : -1;
}

int oracle_is_leader(oracle_node_ctx_t *ctx)
{
    return raft_is_leader(ctx->raft);
}

int oracle_tick(oracle_node_ctx_t *ctx, int elapsed_ms)
{
    return raft_periodic(ctx->raft, elapsed_ms);
}

int oracle_propose_health_transition(oracle_node_ctx_t *ctx,
                                     uint8_t target_node, uint8_t target_box,
                                     uint8_t old_status, uint8_t new_status)
{
    if (!raft_is_leader(ctx->raft))
        return -1; /* RAFT_ERR_NOT_LEADER */

    payload_health_update_t ht;
    memset(&ht, 0, sizeof(ht));
    ht.target_node_id = target_node;
    ht.target_box_id = target_box;
    ht.old_status = old_status;
    ht.new_status = new_status;
    ht.term = (uint32_t)raft_get_current_term(ctx->raft);
    ht.timestamp_ms = (uint32_t)ctx->transport->now_ms(ctx->transport);

    raft_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.term = raft_get_current_term(ctx->raft);
    entry.id = ctx->next_entry_id++;
    entry.type = RAFT_LOGTYPE_NORMAL;
    entry.data.buf = &ht;
    entry.data.len = sizeof(ht);

    msg_entry_response_t response;
    int e = raft_recv_entry(ctx->raft, &entry, &response);
    if (e != 0) {
        printf("raft: propose health transition failed: %d\r\n", e);
    }
    return e;
}

void oracle_destroy(oracle_node_ctx_t *ctx)
{
    if (ctx->raft) {
        raft_free(ctx->raft);
        ctx->raft = NULL;
    }
}

/* --- Message Dispatch (called by simulator/firmware receive loop) --- */

int oracle_dispatch_raft_message(oracle_node_ctx_t *ctx,
                                 uint8_t src_node_id,
                                 raft_msg_type_t msg_type,
                                 const void *payload, size_t len)
{
    raft_node_t *sender = raft_get_node(ctx->raft, src_node_id);
    if (!sender) return -1;

    switch (msg_type) {
    case MSG_REQUEST_VOTE: {
        if (len < sizeof(payload_request_vote_t)) return -1;
        const payload_request_vote_t *p = (const payload_request_vote_t *)payload;

        msg_requestvote_t rv;
        rv.term = raft_get_current_term(ctx->raft); /* sender's term is in header */
        rv.candidate_id = (raft_node_id_t)p->candidate_id;
        rv.last_log_idx = (raft_index_t)p->last_log_idx;
        rv.last_log_term = (raft_term_t)p->last_log_term;

        /* Extract term from the sim header - for now use the raft library's term.
         * In a real implementation, the term comes from the wire header. */
        msg_requestvote_response_t resp;
        int e = raft_recv_requestvote(ctx->raft, sender, &rv, &resp);

        /* Send response back */
        payload_request_vote_resp_t rp;
        rp.vote_granted = (uint32_t)resp.vote_granted;
        rp.current_idx = 0;

        ctx->transport->send(ctx->transport, src_node_id,
                             ETHERTYPE_RAFT, MSG_REQUEST_VOTE_RESP,
                             &rp, sizeof(rp));
        return e;
    }

    case MSG_REQUEST_VOTE_RESP: {
        if (len < sizeof(payload_request_vote_resp_t)) return -1;
        const payload_request_vote_resp_t *p =
            (const payload_request_vote_resp_t *)payload;

        msg_requestvote_response_t resp;
        resp.term = raft_get_current_term(ctx->raft);
        resp.vote_granted = (int)p->vote_granted;

        return raft_recv_requestvote_response(ctx->raft, sender, &resp);
    }

    case MSG_APPEND_ENTRIES: {
        if (len < sizeof(payload_append_entries_t)) return -1;
        const payload_append_entries_t *p =
            (const payload_append_entries_t *)payload;

        msg_appendentries_t ae;
        memset(&ae, 0, sizeof(ae));
        ae.term = raft_get_current_term(ctx->raft);
        ae.prev_log_idx = (raft_index_t)p->prev_log_idx;
        ae.prev_log_term = (raft_term_t)p->prev_log_term;
        ae.leader_commit = (raft_index_t)p->leader_commit;
        ae.n_entries = 0;
        ae.entries = NULL;

        /* Deserialize entries if present */
        msg_entry_t entry;
        if (p->n_entries > 0 &&
            len >= sizeof(payload_append_entries_t) + sizeof(raft_log_entry_wire_t))
        {
            const raft_log_entry_wire_t *we =
                (const raft_log_entry_wire_t *)((const uint8_t *)payload +
                    sizeof(payload_append_entries_t));

            memset(&entry, 0, sizeof(entry));
            entry.term = (raft_term_t)we->term;
            entry.id = 0;
            entry.type = (int)we->entry_type;

            /* Pack health transition data as entry payload */
            entry.data.buf = (void *)&we->target_node_id;
            entry.data.len = sizeof(raft_log_entry_wire_t) -
                             offsetof(raft_log_entry_wire_t, target_node_id);

            ae.n_entries = 1;
            ae.entries = &entry;
        }

        msg_appendentries_response_t resp;
        int e = raft_recv_appendentries(ctx->raft, sender, &ae, &resp);

        /* Send response */
        payload_append_entries_resp_t rp;
        memset(&rp, 0, sizeof(rp));
        rp.success = (uint32_t)resp.success;
        rp.current_idx = (uint32_t)resp.current_idx;
        rp.first_idx = (uint32_t)resp.first_idx;
        rp.n_observations = 0;
        if (ctx->health_ctx) {
            health_monitor_get_observations(
                (health_monitor_t *)ctx->health_ctx,
                rp.obs, &rp.n_observations);
        }

        ctx->transport->send(ctx->transport, src_node_id,
                             ETHERTYPE_RAFT, MSG_APPEND_ENTRIES_RESP,
                             &rp, sizeof(rp));
        return e;
    }

    case MSG_APPEND_ENTRIES_RESP: {
        if (len < 12) return -1; /* at least the 3 standard fields */
        const payload_append_entries_resp_t *p =
            (const payload_append_entries_resp_t *)payload;

        /* Leader-side: process follower health observations */
        if (ctx->health_ctx && p->n_observations > 0 &&
            len >= sizeof(payload_append_entries_resp_t))
        {
            health_monitor_update_corroboration(
                (health_monitor_t *)ctx->health_ctx,
                src_node_id, p->obs, p->n_observations,
                (uint32_t)ctx->transport->now_ms(ctx->transport));
        }

        msg_appendentries_response_t resp;
        resp.term = raft_get_current_term(ctx->raft);
        resp.success = (int)p->success;
        resp.current_idx = (raft_index_t)p->current_idx;
        resp.first_idx = (raft_index_t)p->first_idx;

        return raft_recv_appendentries_response(ctx->raft, sender, &resp);
    }

    default:
        return -1;
    }
}

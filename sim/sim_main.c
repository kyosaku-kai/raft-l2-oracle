/**
 * sim_main.c - POSIX simulator for the L2 Raft oracle
 *
 * Spawns N raft nodes as pthreads, each running the same oracle code
 * that will run on STM32 FreeRTOS tasks. Transport is UDP loopback.
 *
 * Demonstrates: leader election, heartbeat replication, log convergence.
 *
 * Usage: ./raft_sim [num_nodes]    (default: 3)
 */

#include <stddef.h>  /* size_t, needed before raft.h on strict compilers */
#include "../firmware/raft/raft_oracle.h"
#include "../firmware/transport/transport_sim.h"
#include "../firmware/protocol/wire_format.h"

#include "raft.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

#define MAX_SIM_NODES 8
#define SIM_TICK_MS   50
#define SIM_RUNTIME_S 10

static volatile int g_running = 1;

typedef struct {
    oracle_node_ctx_t  oracle;
    raft_transport_t  *transport;
    pthread_t          thread;
    uint8_t            node_id;
    int                num_peers;
    uint8_t            peer_ids[MAX_SIM_NODES];
} sim_node_t;

static sim_node_t g_nodes[MAX_SIM_NODES];
static int g_num_nodes = 3;

static void sigint_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/**
 * Per-node thread: initialize raft, then run tick loop.
 */
static void *node_thread(void *arg)
{
    sim_node_t *sn = (sim_node_t *)arg;
    oracle_node_ctx_t *ctx = &sn->oracle;

    /* Seed rand differently per node to avoid election split-votes */
    srand((unsigned)(sn->node_id * 31337 + time(NULL)));

    /* Initialize oracle */
    if (oracle_init(ctx, sn->node_id, sn->node_id /* box_id = node_id for sim */,
                    sn->transport) != 0) {
        fprintf(stderr, "[node %d] oracle_init failed\n", sn->node_id);
        return NULL;
    }

    /* Add self and all peers */
    oracle_add_node(ctx, sn->node_id, 1 /* is_self */);
    for (int i = 0; i < sn->num_peers; i++) {
        oracle_add_node(ctx, sn->peer_ids[i], 0);
    }

    printf("[node %d] started (port %d)\n", sn->node_id,
           SIM_BASE_PORT + sn->node_id);

    /* Main loop: drain messages, tick raft */
    uint64_t last_tick = ctx->transport->now_ms(ctx->transport);
    uint64_t last_status = last_tick;

    while (g_running) {
        uint64_t now = ctx->transport->now_ms(ctx->transport);

        /* Drain inbound messages (non-blocking) */
        for (int i = 0; i < 10; i++) {
            uint16_t ethertype;
            raft_msg_type_t type;
            uint8_t src_node;
            uint8_t payload[128];

            int received = ctx->transport->recv(ctx->transport, &ethertype,
                                                &type, &src_node,
                                                payload, sizeof(payload), 0);
            if (received <= 0) break;

            oracle_dispatch_raft_message(ctx, src_node, type,
                                         payload, (size_t)received);
        }

        /* Raft periodic tick */
        uint64_t elapsed = now - last_tick;
        if (elapsed >= SIM_TICK_MS) {
            raft_periodic(ctx->raft, (int)elapsed);
            last_tick = now;
        }

        /* Status report every second */
        if (now - last_status >= 1000) {
            const char *state_str = "???";
            if (raft_is_leader(ctx->raft))
                state_str = "LEADER";
            else if (raft_is_candidate(ctx->raft))
                state_str = "CANDIDATE";
            else if (raft_is_follower(ctx->raft))
                state_str = "FOLLOWER";

            raft_node_id_t leader = raft_get_current_leader(ctx->raft);

            printf("[node %d] state=%s term=%ld leader=%d commit_idx=%ld\n",
                   sn->node_id, state_str,
                   raft_get_current_term(ctx->raft),
                   (int)leader,
                   raft_get_commit_idx(ctx->raft));

            last_status = now;
        }

        /* Sleep briefly to avoid busy-spinning */
        usleep(5000); /* 5ms */
    }

    printf("[node %d] shutting down\n", sn->node_id);
    oracle_destroy(ctx);
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc > 1)
        g_num_nodes = atoi(argv[1]);
    if (g_num_nodes < 2 || g_num_nodes > MAX_SIM_NODES) {
        fprintf(stderr, "Usage: %s [num_nodes (2-%d)]\n", argv[0], MAX_SIM_NODES);
        return 1;
    }

    printf("=== raft-l2-oracle POSIX simulator ===\n");
    printf("Nodes: %d, tick: %dms, runtime: %ds\n\n",
           g_num_nodes, SIM_TICK_MS, SIM_RUNTIME_S);

    signal(SIGINT, sigint_handler);

    /* Create transports (must be done before threads to bind ports) */
    for (int i = 0; i < g_num_nodes; i++) {
        uint8_t node_id = (uint8_t)(i + 1);
        g_nodes[i].node_id = node_id;
        g_nodes[i].transport = sim_transport_create(node_id, node_id);
        if (!g_nodes[i].transport) {
            fprintf(stderr, "Failed to create transport for node %d\n", node_id);
            return 1;
        }

        /* Build peer list (all other nodes) */
        g_nodes[i].num_peers = 0;
        for (int j = 0; j < g_num_nodes; j++) {
            if (j != i) {
                g_nodes[i].peer_ids[g_nodes[i].num_peers++] =
                    (uint8_t)(j + 1);
            }
        }
    }

    /* Launch node threads */
    for (int i = 0; i < g_num_nodes; i++) {
        if (pthread_create(&g_nodes[i].thread, NULL, node_thread,
                           &g_nodes[i]) != 0) {
            fprintf(stderr, "Failed to create thread for node %d\n",
                    g_nodes[i].node_id);
            return 1;
        }
    }

    /* Run for SIM_RUNTIME_S seconds then stop */
    for (int s = 0; s < SIM_RUNTIME_S && g_running; s++) {
        sleep(1);
    }
    g_running = 0;

    /* Join threads */
    for (int i = 0; i < g_num_nodes; i++) {
        pthread_join(g_nodes[i].thread, NULL);
        sim_transport_destroy(g_nodes[i].transport);
    }

    printf("\n=== simulation complete ===\n");
    return 0;
}

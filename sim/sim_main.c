/**
 * sim_main.c - POSIX simulator for the L2 Raft oracle
 *
 * Spawns N raft nodes as pthreads, each running the same oracle code
 * that will run on STM32 FreeRTOS tasks. Transport is UDP loopback.
 *
 * Each node also has a simulated compute node sending heartbeats.
 * Use --chaos to inject heartbeat failure after 3 seconds.
 *
 * Usage: ./raft_sim [num_nodes] [--chaos]
 */

#include <stddef.h>  /* size_t, needed before raft.h on strict compilers */
#include "../firmware/raft/raft_oracle.h"
#include "../firmware/transport/transport_sim.h"
#include "../firmware/protocol/wire_format.h"
#include "../firmware/health/health_monitor.h"

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
#define SIM_RUNTIME_S  10
#define SIM_CHAOS_RUNTIME_S 15
#define CHAOS_KILL_AT_S 3   /* seconds into sim when chaos kills heartbeats */
#define CHAOS_LEADER_KILL_S 8 /* seconds into sim when chaos kills leader */

static volatile int g_running = 1;
static int g_chaos = 0;

typedef struct {
    oracle_node_ctx_t  oracle;
    health_monitor_t   health;
    raft_transport_t  *transport;
    pthread_t          thread;
    uint8_t            node_id;
    int                num_peers;
    uint8_t            peer_ids[MAX_SIM_NODES];
    volatile int       alive;  /* 0 = stop this node (chaos: leader kill) */
} sim_node_t;

/* Simulated compute node (sends heartbeats to its local STM32) */
typedef struct {
    pthread_t          thread;
    uint8_t            compute_node_id;  /* assigned ID in health table */
    uint8_t            stm32_node_id;    /* which STM32 to send to */
    raft_transport_t  *transport;        /* shared with STM32 node (send only) */
    volatile int       alive;            /* 0 = stop sending heartbeats */
} sim_compute_t;

static sim_node_t g_nodes[MAX_SIM_NODES];
static sim_compute_t g_computes[MAX_SIM_NODES];
static int g_num_nodes = 3;

static void sigint_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/**
 * Simulated compute node thread: sends heartbeats every 10ms.
 */
static void *compute_thread(void *arg)
{
    sim_compute_t *cn = (sim_compute_t *)arg;
    uint32_t seq = 0;

    /* First, send an announce so the health monitor registers us */
    payload_node_announce_t ann;
    memset(&ann, 0, sizeof(ann));
    ann.node_type = NODE_TYPE_X86;
    memcpy(ann.hostname, "sim", 3);

    /* Broadcast announce to all STM32 nodes (L2 broadcast on real hardware) */
    cn->transport->send(cn->transport, 0xFF,
                        ETHERTYPE_HEARTBEAT, MSG_NODE_ANNOUNCE, 0,
                        &ann, sizeof(ann));

    /* Wait a bit for the announce to be processed */
    usleep(50000);

    printf("[compute %d] heartbeat sender started (-> stm32 node %d)\n",
           cn->compute_node_id, cn->stm32_node_id);

    while (g_running && cn->alive) {
        payload_node_heartbeat_t hb;
        memset(&hb, 0, sizeof(hb));
        hb.seq = seq++;
        hb.load_pct = 150; /* 15.0% simulated load */

        /* Broadcast to ALL STM32 nodes (matches L2 broadcast on real hardware).
         * On a flat Ethernet bridge, heartbeats from any compute node reach
         * every STM32, so all nodes can observe the heartbeat and participate
         * in corroboration. */
        cn->transport->send(cn->transport, 0xFF,
                            ETHERTYPE_HEARTBEAT, MSG_NODE_HEARTBEAT, 0,
                            &hb, sizeof(hb));

        usleep(HB_INTERVAL_MS * 1000);
    }

    if (!cn->alive) {
        printf("[compute %d] KILLED - heartbeats stopped\n",
               cn->compute_node_id);
    }

    return NULL;
}

/**
 * Per-node thread: initialize raft + health monitor, then run tick loop.
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

    /* Initialize health monitor and link to oracle */
    health_monitor_init(&sn->health,
                        (struct oracle_node_ctx *)ctx,
                        sn->transport, sn->node_id);
    ctx->health_ctx = &sn->health;

    /* Add self and all peers */
    oracle_add_node(ctx, sn->node_id, 1 /* is_self */);
    for (int i = 0; i < sn->num_peers; i++) {
        oracle_add_node(ctx, sn->peer_ids[i], 0);
    }

    printf("[node %d] started (port %d)\n", sn->node_id,
           SIM_BASE_PORT + sn->node_id);

    /* Main loop: drain messages, tick raft + health */
    uint64_t last_tick = ctx->transport->now_ms(ctx->transport);
    uint64_t last_status = last_tick;
    uint64_t last_health_tick = last_tick;

    while (g_running && sn->alive) {
        uint64_t now_val = ctx->transport->now_ms(ctx->transport);

        /* Drain inbound messages (non-blocking) */
        for (int i = 0; i < 20; i++) {
            uint16_t ethertype;
            raft_msg_type_t type;
            uint8_t src_node;
            uint32_t msg_term;
            uint8_t payload[128];

            int received = ctx->transport->recv(ctx->transport, &ethertype,
                                                &type, &src_node, &msg_term,
                                                payload, sizeof(payload), 0);
            if (received <= 0) break;

            if (ethertype == ETHERTYPE_RAFT) {
                oracle_dispatch_raft_message(ctx, src_node, type, msg_term,
                                             payload, (size_t)received);
            } else if (ethertype == ETHERTYPE_HEARTBEAT) {
                if (type == MSG_NODE_HEARTBEAT &&
                    (size_t)received >= sizeof(payload_node_heartbeat_t))
                {
                    health_monitor_process_heartbeat(
                        &sn->health, src_node,
                        (const payload_node_heartbeat_t *)payload,
                        (uint32_t)now_val);
                }
                else if (type == MSG_NODE_ANNOUNCE &&
                         (size_t)received >= sizeof(payload_node_announce_t))
                {
                    health_monitor_process_announce(
                        &sn->health, src_node,
                        (const payload_node_announce_t *)payload,
                        (uint32_t)now_val);
                }
            }
        }

        /* Raft periodic tick */
        uint64_t elapsed = now_val - last_tick;
        if (elapsed >= SIM_TICK_MS) {
            raft_periodic(ctx->raft, (int)elapsed);
            last_tick = now_val;
        }

        /* Health monitor tick (~10ms) */
        if (now_val - last_health_tick >= HB_INTERVAL_MS) {
            health_monitor_tick(&sn->health, (uint32_t)now_val);
            last_health_tick = now_val;
        }

        /* Broadcast cluster state every second (leader only) */
        if (now_val - last_status >= 1000) {
            health_table_broadcast_cluster_state(&sn->health, ctx);
        }

        /* Status report every second */
        if (now_val - last_status >= 1000) {
            const char *state_str = "???";
            if (raft_is_leader(ctx->raft))
                state_str = "LEADER";
            else if (raft_is_candidate(ctx->raft))
                state_str = "CANDIDATE";
            else if (raft_is_follower(ctx->raft))
                state_str = "FOLLOWER";

            raft_node_id_t leader = raft_get_current_leader(ctx->raft);
            raft_index_t ci = raft_get_commit_idx(ctx->raft);

            /* Build entire line in buffer for atomic write (no interleaving) */
            char line[256];
            int pos = snprintf(line, sizeof(line),
                "[node %d] state=%s term=%ld leader=%d commit=%ld",
                sn->node_id, state_str,
                raft_get_current_term(ctx->raft),
                (int)leader, ci);

            /* Append health table */
            uint8_t n_health;
            const node_health_entry_t *tbl =
                health_monitor_get_table(&sn->health, &n_health);
            if (n_health > 0 && pos < (int)sizeof(line) - 1) {
                pos += snprintf(line + pos, sizeof(line) - pos, " | health:");
                for (uint8_t h = 0; h < n_health && pos < (int)sizeof(line) - 1; h++) {
                    const char *st = "?";
                    switch (tbl[h].status) {
                    case NODE_UP:      st = "UP";      break;
                    case NODE_SUSPECT: st = "SUSPECT"; break;
                    case NODE_DOWN:    st = "DOWN";    break;
                    }
                    pos += snprintf(line + pos, sizeof(line) - pos,
                                    " n%d=%s", tbl[h].node_id, st);
                }
            }
            if (pos < (int)sizeof(line) - 1)
                line[pos++] = '\n';
            line[pos] = '\0';
            fputs(line, stdout);

            last_status = now_val;
        }

        /* Sleep briefly to avoid busy-spinning */
        usleep(1000); /* 1ms - faster than before for health responsiveness */
    }

    printf("[node %d] shutting down\n", sn->node_id);
    oracle_destroy(ctx);
    return NULL;
}

int main(int argc, char *argv[])
{
    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--chaos") == 0) {
            g_chaos = 1;
        } else {
            int n = atoi(argv[i]);
            if (n >= 2 && n <= MAX_SIM_NODES)
                g_num_nodes = n;
        }
    }

    if (g_num_nodes < 2 || g_num_nodes > MAX_SIM_NODES) {
        fprintf(stderr, "Usage: %s [num_nodes (2-%d)] [--chaos]\n",
                argv[0], MAX_SIM_NODES);
        return 1;
    }

    printf("=== raft-l2-oracle POSIX simulator ===\n");
    int runtime = g_chaos ? SIM_CHAOS_RUNTIME_S : SIM_RUNTIME_S;
    printf("Nodes: %d, tick: %dms, runtime: %ds%s\n\n",
           g_num_nodes, SIM_TICK_MS, runtime,
           g_chaos ? ", CHAOS MODE" : "");

    signal(SIGINT, sigint_handler);

    /* Create transports (must be done before threads to bind ports) */
    uint8_t all_node_ids[MAX_SIM_NODES];
    for (int i = 0; i < g_num_nodes; i++) {
        uint8_t node_id = (uint8_t)(i + 1);
        all_node_ids[i] = node_id;
        g_nodes[i].node_id = node_id;
        g_nodes[i].alive = 1;
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

    /* Register broadcast peers so dst_node=0xFF reaches all nodes */
    sim_transport_set_broadcast_peers(all_node_ids, g_num_nodes);

    /* Launch node threads */
    for (int i = 0; i < g_num_nodes; i++) {
        if (pthread_create(&g_nodes[i].thread, NULL, node_thread,
                           &g_nodes[i]) != 0) {
            fprintf(stderr, "Failed to create thread for node %d\n",
                    g_nodes[i].node_id);
            return 1;
        }
    }

    /* Wait for nodes to initialize before starting compute heartbeats */
    usleep(500000); /* 500ms for raft to settle */

    /* Create simulated compute node transports and threads.
     * Each STM32 node gets one simulated compute node.
     * Compute nodes use node IDs 100+i to avoid collision with STM32 IDs.
     * Each compute gets its OWN transport so src_node in wire header
     * correctly identifies the compute node (matching real hardware where
     * the compute node's MAC identifies it). */
    for (int i = 0; i < g_num_nodes; i++) {
        uint8_t compute_id = (uint8_t)(100 + i + 1);
        g_computes[i].compute_node_id = compute_id;
        g_computes[i].stm32_node_id = (uint8_t)(i + 1);
        g_computes[i].transport = sim_transport_create(compute_id, 1);
        if (!g_computes[i].transport) {
            fprintf(stderr, "Failed to create compute transport %d\n", i);
            continue;
        }
        g_computes[i].alive = 1;

        if (pthread_create(&g_computes[i].thread, NULL, compute_thread,
                           &g_computes[i]) != 0) {
            fprintf(stderr, "Failed to create compute thread %d\n", i);
        }
    }

    /* Run simulation */
    for (int s = 0; s < runtime && g_running; s++) {
        sleep(1);

        /* Chaos injection: kill heartbeats for compute node 0 at CHAOS_KILL_AT_S */
        if (g_chaos && s == CHAOS_KILL_AT_S) {
            printf("\n!!! CHAOS: killing heartbeats for compute node %d !!!\n\n",
                   g_computes[0].compute_node_id);
            g_computes[0].alive = 0;
        }

        /* Chaos injection: kill the leader at CHAOS_LEADER_KILL_S */
        if (g_chaos && s == CHAOS_LEADER_KILL_S) {
            for (int i = 0; i < g_num_nodes; i++) {
                if (g_nodes[i].alive &&
                    raft_is_leader(g_nodes[i].oracle.raft))
                {
                    printf("\n!!! CHAOS: killing LEADER node %d !!!\n\n",
                           g_nodes[i].node_id);
                    g_nodes[i].alive = 0;
                    break;
                }
            }
        }
    }
    g_running = 0;

    /* Stop all compute threads */
    for (int i = 0; i < g_num_nodes; i++) {
        g_computes[i].alive = 0;
    }
    for (int i = 0; i < g_num_nodes; i++) {
        pthread_join(g_computes[i].thread, NULL);
        sim_transport_destroy(g_computes[i].transport);
    }

    /* Join node threads */
    for (int i = 0; i < g_num_nodes; i++) {
        pthread_join(g_nodes[i].thread, NULL);
        sim_transport_destroy(g_nodes[i].transport);
    }

    printf("\n=== simulation complete ===\n");
    return 0;
}

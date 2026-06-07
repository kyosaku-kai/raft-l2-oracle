/**
 * transport_sim.c - POSIX UDP loopback transport
 *
 * Each node listens on 127.0.0.1:<SIM_BASE_PORT + node_id>.
 * Sends are UDP datagrams containing: oracle_frame_header_t (10 bytes,
 * no Ethernet header) + payload.
 *
 * The Ethernet MAC header (dst/src MAC, EtherType) is omitted since
 * UDP already handles addressing. We send only the oracle protocol
 * header (version through payload_len) plus the payload.
 */

#include "transport_sim.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

/** Sim-specific wire header (no Ethernet MACs, just oracle fields) */
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint8_t  node_id;
    uint8_t  box_id;
    uint32_t term;
    uint16_t payload_len;
    uint16_t ethertype;     /* propagate EtherType for health/heartbeat demux */
} sim_header_t;

_Static_assert(sizeof(sim_header_t) == 12, "sim header must be 12 bytes");

typedef struct {
    int       sock_fd;
    uint8_t   node_id;
    uint8_t   box_id;
} sim_transport_data_t;

static uint64_t sim_now_ms(raft_transport_t *t)
{
    (void)t;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int sim_send(raft_transport_t *t, uint8_t dst_node, uint16_t ethertype,
                    raft_msg_type_t type, uint32_t term,
                    const void *payload, size_t len)
{
    sim_transport_data_t *sd = (sim_transport_data_t *)t->impl_data;

    uint8_t buf[sizeof(sim_header_t) + ORACLE_MAX_PAYLOAD_V1];
    sim_header_t *hdr = (sim_header_t *)buf;

    if (len > ORACLE_MAX_PAYLOAD_V1)
        return -1;

    hdr->version = ORACLE_PROTOCOL_VERSION;
    hdr->msg_type = (uint8_t)type;
    hdr->node_id = sd->node_id;
    hdr->box_id = sd->box_id;
    hdr->term = term;
    hdr->payload_len = (uint16_t)len;
    hdr->ethertype = ethertype;

    if (len > 0)
        memcpy(buf + sizeof(sim_header_t), payload, len);

    struct sockaddr_in dst_addr;
    memset(&dst_addr, 0, sizeof(dst_addr));
    dst_addr.sin_family = AF_INET;
    dst_addr.sin_port = htons(SIM_BASE_PORT + dst_node);
    dst_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    ssize_t sent = sendto(sd->sock_fd, buf, sizeof(sim_header_t) + len, 0,
                          (struct sockaddr *)&dst_addr, sizeof(dst_addr));
    return (sent > 0) ? 0 : -1;
}

static int sim_recv(raft_transport_t *t, uint16_t *ethertype,
                    raft_msg_type_t *type, uint8_t *src_node, uint32_t *term,
                    void *payload, size_t max_len, uint32_t timeout_ms)
{
    sim_transport_data_t *sd = (sim_transport_data_t *)t->impl_data;

    struct pollfd pfd = { .fd = sd->sock_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, (int)timeout_ms);
    if (ret <= 0)
        return ret; /* 0 = timeout, -1 = error */

    uint8_t buf[sizeof(sim_header_t) + ORACLE_MAX_PAYLOAD_V1];
    ssize_t received = recvfrom(sd->sock_fd, buf, sizeof(buf), 0, NULL, NULL);
    if (received < (ssize_t)sizeof(sim_header_t))
        return -1;

    sim_header_t *hdr = (sim_header_t *)buf;

    /* Version check */
    if ((hdr->version & 0xF0) != (ORACLE_PROTOCOL_VERSION & 0xF0))
        return -1;

    *type = (raft_msg_type_t)hdr->msg_type;
    *src_node = hdr->node_id;
    *ethertype = hdr->ethertype;
    *term = hdr->term;

    size_t payload_len = hdr->payload_len;
    if (payload_len > max_len)
        payload_len = max_len;

    if (payload_len > 0)
        memcpy(payload, buf + sizeof(sim_header_t), payload_len);

    return (int)payload_len;
}

raft_transport_t *sim_transport_create(uint8_t node_id, uint8_t box_id)
{
    raft_transport_t *t = calloc(1, sizeof(raft_transport_t));
    sim_transport_data_t *sd = calloc(1, sizeof(sim_transport_data_t));
    if (!t || !sd) {
        free(t);
        free(sd);
        return NULL;
    }

    sd->node_id = node_id;
    sd->box_id = box_id;

    /* Create UDP socket */
    sd->sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sd->sock_fd < 0) {
        free(t);
        free(sd);
        return NULL;
    }

    /* Allow address reuse for quick restart */
    int reuse = 1;
    setsockopt(sd->sock_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* Bind to 127.0.0.1:<base_port + node_id> */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SIM_BASE_PORT + node_id);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(sd->sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sd->sock_fd);
        free(t);
        free(sd);
        return NULL;
    }

    t->send = sim_send;
    t->recv = sim_recv;
    t->now_ms = sim_now_ms;
    t->impl_data = sd;

    return t;
}

void sim_transport_destroy(raft_transport_t *t)
{
    if (!t) return;
    sim_transport_data_t *sd = (sim_transport_data_t *)t->impl_data;
    if (sd) {
        if (sd->sock_fd >= 0)
            close(sd->sock_fd);
        free(sd);
    }
    free(t);
}

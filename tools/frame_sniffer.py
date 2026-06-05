#!/usr/bin/env python3
"""L2 Oracle frame sniffer - captures and decodes raw Ethernet oracle frames.

Captures EtherType 0x88B5 (Raft), 0x88B6 (Health), 0x88B7 (Heartbeat)
frames and prints decoded contents. Requires root (raw socket).

Usage:
    sudo python3 tools/frame_sniffer.py eth0
    sudo python3 tools/frame_sniffer.py eth0 --type raft
    sudo python3 tools/frame_sniffer.py eth0 --type heartbeat --pcap capture.pcap
"""

import argparse
import struct
import sys
import time
from datetime import datetime

from scapy.all import Ether, Raw, sniff, wrpcap

# Wire protocol constants (from firmware/protocol/wire_format.h)
ETHERTYPE_RAFT      = 0x88B5
ETHERTYPE_HEALTH    = 0x88B6
ETHERTYPE_HEARTBEAT = 0x88B7

ETHERTYPES = {
    ETHERTYPE_RAFT:      "RAFT",
    ETHERTYPE_HEALTH:    "HEALTH",
    ETHERTYPE_HEARTBEAT: "HEARTBEAT",
}

MSG_TYPES = {
    0x01: "REQUEST_VOTE",
    0x02: "REQUEST_VOTE_RESP",
    0x03: "APPEND_ENTRIES",
    0x04: "APPEND_ENTRIES_RESP",
    0x10: "HEALTH_UPDATE",
    0x11: "CLUSTER_STATE",
    0x12: "FAILURE_EVENT",
    0x20: "NODE_HEARTBEAT",
    0x21: "NODE_ANNOUNCE",
    0x22: "NODE_ANNOUNCE_ACK",
}

NODE_STATUS = {0: "UP", 1: "SUSPECT", 2: "DOWN"}


def decode_oracle_header(data):
    """Decode 10-byte oracle protocol header (after 14-byte Ethernet header)."""
    if len(data) < 10:
        return None
    version, msg_type, node_id, box_id, term, payload_len = struct.unpack_from(
        "<BBBBIh", data, 0
    )
    return {
        "version": (version >> 4) & 0xF,
        "msg_type": msg_type,
        "msg_name": MSG_TYPES.get(msg_type, f"0x{msg_type:02x}"),
        "node_id": node_id,
        "box_id": box_id,
        "term": term,
        "payload_len": payload_len,
    }


def decode_payload(msg_type, data):
    """Decode message-specific payload."""
    if msg_type == 0x01 and len(data) >= 16:  # REQUEST_VOTE
        cid, last_idx, last_term, _ = struct.unpack_from("<IIII", data)
        return f"candidate={cid} last_idx={last_idx} last_term={last_term}"

    if msg_type == 0x02 and len(data) >= 8:  # REQUEST_VOTE_RESP
        granted, cur_idx = struct.unpack_from("<II", data)
        return f"granted={'yes' if granted else 'no'} cur_idx={cur_idx}"

    if msg_type == 0x03 and len(data) >= 16:  # APPEND_ENTRIES
        prev_idx, prev_term, leader_commit, n_entries = struct.unpack_from("<IIIB", data)
        kind = "heartbeat" if n_entries == 0 else f"{n_entries} entries"
        return f"prev={prev_idx}/{prev_term} commit={leader_commit} [{kind}]"

    if msg_type == 0x04 and len(data) >= 16:  # APPEND_ENTRIES_RESP
        success, cur_idx, first_idx, n_obs = struct.unpack_from("<IIIB", data)
        s = f"{'ok' if success else 'FAIL'} cur={cur_idx} first={first_idx}"
        if n_obs > 0:
            s += f" obs={n_obs}"
            offset = 16
            for i in range(min(n_obs, 8)):
                if offset + 4 <= len(data):
                    nid, status, conf, _ = struct.unpack_from("<BBBB", data, offset)
                    s += f" [{nid}:{NODE_STATUS.get(status, '?')} c={conf}]"
                    offset += 4
        return s

    if msg_type in (0x10, 0x12) and len(data) >= 12:  # HEALTH_UPDATE/FAILURE_EVENT
        tgt_node, tgt_box, old_st, new_st, term, ts = struct.unpack_from("<BBBBII", data)
        return (
            f"node={tgt_node} box={tgt_box} "
            f"{NODE_STATUS.get(old_st, '?')}->{NODE_STATUS.get(new_st, '?')} "
            f"term={term} t={ts}ms"
        )

    if msg_type == 0x11 and len(data) >= 4:  # CLUSTER_STATE
        n_nodes, leader_nid, leader_bid, quorum = struct.unpack_from("<BBBB", data)
        s = f"nodes={n_nodes} leader={leader_nid}/{leader_bid} quorum={'yes' if quorum else 'no'}"
        offset = 4
        for i in range(min(n_nodes, 16)):
            if offset + 12 <= len(data):
                nid, bid, ntype, status, last_seen, st_term = struct.unpack_from(
                    "<BBBBII", data, offset
                )
                s += f"\n    [{nid}] box={bid} {NODE_STATUS.get(status, '?')} seen={last_seen}ms"
                offset += 12
        return s

    if msg_type == 0x20 and len(data) >= 8:  # NODE_HEARTBEAT
        seq, load, _ = struct.unpack_from("<IHH", data)
        return f"seq={seq} load={load / 10:.1f}%"

    if msg_type == 0x21 and len(data) >= 16:  # NODE_ANNOUNCE
        ntype = data[0]
        mac = data[4:10]
        hostname = data[10:16]
        types = {1: "JETSON", 2: "X86", 3: "STM32"}
        return (
            f"type={types.get(ntype, ntype)} "
            f"mac={':'.join(f'{b:02x}' for b in mac)} "
            f"host={hostname.decode('ascii', errors='replace').rstrip(chr(0))}"
        )

    if msg_type == 0x22 and len(data) >= 4:  # NODE_ANNOUNCE_ACK
        nid, bid, status, _ = struct.unpack_from("<BBBB", data)
        return f"assigned_id={nid} box={bid} status={NODE_STATUS.get(status, '?')}"

    return data.hex() if data else ""


def process_packet(pkt, verbose=False):
    """Process and print a captured oracle frame."""
    if not pkt.haslayer(Ether):
        return

    eth = pkt[Ether]
    etype = eth.type

    if etype not in ETHERTYPES:
        return

    ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    etype_name = ETHERTYPES[etype]
    raw = bytes(eth.payload) if eth.payload else b""

    hdr = decode_oracle_header(raw)
    if not hdr:
        print(f"{ts} [{etype_name}] {eth.src} -> {eth.dst}  (truncated header)")
        return

    payload_data = raw[10 : 10 + hdr["payload_len"]]
    payload_str = decode_payload(hdr["msg_type"], payload_data)

    print(
        f"{ts} [{etype_name:9s}] "
        f"node={hdr['node_id']} box={hdr['box_id']} "
        f"term={hdr['term']:>4d} "
        f"{hdr['msg_name']:20s} "
        f"{payload_str}"
    )

    if verbose and payload_data:
        print(f"           raw: {payload_data.hex()}")


def main():
    parser = argparse.ArgumentParser(description="L2 Oracle frame sniffer")
    parser.add_argument("iface", help="Network interface to sniff on")
    parser.add_argument(
        "--type",
        choices=["raft", "health", "heartbeat", "all"],
        default="all",
        help="Filter by frame type (default: all)",
    )
    parser.add_argument("--pcap", help="Save captured frames to pcap file")
    parser.add_argument("-v", "--verbose", action="store_true", help="Show raw hex")
    parser.add_argument(
        "-c", "--count", type=int, default=0, help="Stop after N frames (0=unlimited)"
    )
    args = parser.parse_args()

    # Build BPF filter for the selected EtherTypes
    type_map = {
        "raft":      [ETHERTYPE_RAFT],
        "health":    [ETHERTYPE_HEALTH],
        "heartbeat": [ETHERTYPE_HEARTBEAT],
        "all":       [ETHERTYPE_RAFT, ETHERTYPE_HEALTH, ETHERTYPE_HEARTBEAT],
    }
    ethertypes = type_map[args.type]
    bpf = " or ".join(f"ether proto 0x{e:04x}" for e in ethertypes)

    print(f"Sniffing on {args.iface} for {args.type} frames (BPF: {bpf})")
    print(f"{'='*80}")

    captured = []

    def handler(pkt):
        process_packet(pkt, verbose=args.verbose)
        if args.pcap:
            captured.append(pkt)

    try:
        sniff(
            iface=args.iface,
            filter=bpf,
            prn=handler,
            count=args.count if args.count > 0 else 0,
            store=0,
        )
    except KeyboardInterrupt:
        pass
    finally:
        if args.pcap and captured:
            wrpcap(args.pcap, captured)
            print(f"\nSaved {len(captured)} frames to {args.pcap}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Send L2 heartbeat frames to STM32 oracle, simulating a compute node.

Sends EtherType 0x88B7 MSG_NODE_HEARTBEAT frames at configurable intervals.
Can also send MSG_NODE_ANNOUNCE to register the node. Requires root.

Usage:
    # Announce + heartbeat at 10ms (default)
    sudo python3 tools/heartbeat_sender.py eth0

    # Custom node/box ID, 50ms interval
    sudo python3 tools/heartbeat_sender.py eth0 --node-id 101 --box-id 1 --interval 50

    # Just announce, no heartbeats (for testing announce/ack)
    sudo python3 tools/heartbeat_sender.py eth0 --announce-only

    # Stop after 5 seconds (for testing SUSPECT/DOWN detection)
    sudo python3 tools/heartbeat_sender.py eth0 --duration 5
"""

import argparse
import socket
import struct
import sys
import time

# Wire protocol constants
ETHERTYPE_HEARTBEAT = 0x88B7
ORACLE_PROTOCOL_VERSION = 0x10

MSG_NODE_HEARTBEAT   = 0x20
MSG_NODE_ANNOUNCE    = 0x21
MSG_NODE_ANNOUNCE_ACK = 0x22

MAC_BROADCAST = b"\xff\xff\xff\xff\xff\xff"


def oracle_mac(box_id):
    """Build STM32 MAC: 02:CA:FE:<box>:00:01"""
    return bytes([0x02, 0xCA, 0xFE, box_id, 0x00, 0x01])


def compute_mac(box_id, node_id):
    """Build compute node MAC: 02:CA:FE:<box>:00:<node_id>.
    Uses node_id as role byte to distinguish from STM32 (role=0x01)."""
    return bytes([0x02, 0xCA, 0xFE, box_id, 0x00, node_id & 0xFF])


def build_frame(dst_mac, src_mac, ethertype, msg_type, node_id, box_id, term, payload):
    """Build a complete oracle Ethernet frame."""
    # Ethernet header (14 bytes)
    frame = dst_mac + src_mac + struct.pack("!H", ethertype)
    # Oracle protocol header (10 bytes)
    frame += struct.pack("<BBBBIN", ORACLE_PROTOCOL_VERSION, msg_type,
                         node_id, box_id, term, len(payload))
    # Payload
    frame += payload
    # Pad to minimum Ethernet frame size (60 bytes without FCS)
    if len(frame) < 60:
        frame += b"\x00" * (60 - len(frame))
    return frame


def build_heartbeat(seq, load_pct=0):
    """Build MSG_NODE_HEARTBEAT payload (8 bytes)."""
    return struct.pack("<IHH", seq, load_pct, 0)


def build_announce(node_type, mac, hostname):
    """Build MSG_NODE_ANNOUNCE payload (16 bytes)."""
    payload = struct.pack("<B3s", node_type, b"\x00" * 3)
    payload += mac[:6]
    name_bytes = hostname.encode("ascii")[:6].ljust(6, b"\x00")
    payload += name_bytes
    return payload


def main():
    parser = argparse.ArgumentParser(description="Send L2 heartbeats to STM32 oracle")
    parser.add_argument("iface", help="Network interface")
    parser.add_argument("--node-id", type=int, default=101,
                        help="Compute node ID (default: 101)")
    parser.add_argument("--box-id", type=int, default=1,
                        help="Box ID (default: 1, same box as STM32)")
    parser.add_argument("--interval", type=int, default=10,
                        help="Heartbeat interval in ms (default: 10)")
    parser.add_argument("--duration", type=float, default=0,
                        help="Stop after N seconds (0=unlimited)")
    parser.add_argument("--announce-only", action="store_true",
                        help="Send announce then exit")
    parser.add_argument("--no-announce", action="store_true",
                        help="Skip initial announce, just heartbeat")
    parser.add_argument("--node-type", type=int, default=2,
                        help="Node type: 1=JETSON, 2=X86 (default: 2)")
    parser.add_argument("--load", type=int, default=0,
                        help="Simulated CPU load 0-1000 (0.1%% units)")
    args = parser.parse_args()

    src_mac = compute_mac(args.box_id, args.node_id)
    dst_mac = oracle_mac(args.box_id)

    src_mac_str = ":".join(f"{b:02x}" for b in src_mac)
    dst_mac_str = ":".join(f"{b:02x}" for b in dst_mac)

    # Open raw socket
    try:
        sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETHERTYPE_HEARTBEAT))
        sock.bind((args.iface, 0))
    except PermissionError:
        print("Error: requires root (sudo)", file=sys.stderr)
        sys.exit(1)
    except OSError as e:
        print(f"Error: {e} (is interface '{args.iface}' correct?)", file=sys.stderr)
        sys.exit(1)

    print(f"Interface: {args.iface}")
    print(f"Node: id={args.node_id} box={args.box_id} type={args.node_type}")
    print(f"MAC: {src_mac_str} -> {dst_mac_str}")

    # Send announce
    if not args.no_announce:
        hostname = socket.gethostname()
        announce_payload = build_announce(args.node_type, src_mac, hostname)
        frame = build_frame(dst_mac, src_mac, ETHERTYPE_HEARTBEAT,
                            MSG_NODE_ANNOUNCE, args.node_id, args.box_id,
                            0, announce_payload)
        sock.send(frame)
        print(f"Sent NODE_ANNOUNCE (type={args.node_type}, host={hostname[:6]})")

        if args.announce_only:
            sock.close()
            return

    # Heartbeat loop
    interval_s = args.interval / 1000.0
    deadline = time.monotonic() + args.duration if args.duration > 0 else None
    seq = 0

    print(f"Sending heartbeats every {args.interval}ms"
          + (f" for {args.duration}s" if deadline else "")
          + " (Ctrl-C to stop)")

    try:
        while True:
            if deadline and time.monotonic() >= deadline:
                print(f"\nDuration reached ({args.duration}s), stopping heartbeats.")
                print("STM32 should detect SUSPECT in ~30ms, DOWN in ~80ms.")
                # Keep process alive so user can observe detection
                print("Press Ctrl-C to exit.")
                while True:
                    time.sleep(1)

            payload = build_heartbeat(seq, args.load)
            frame = build_frame(dst_mac, src_mac, ETHERTYPE_HEARTBEAT,
                                MSG_NODE_HEARTBEAT, args.node_id, args.box_id,
                                0, payload)
            sock.send(frame)
            seq += 1

            if seq % (1000 // max(args.interval, 1)) == 0:
                print(f"  sent {seq} heartbeats")

            time.sleep(interval_s)
    except KeyboardInterrupt:
        print(f"\nStopped after {seq} heartbeats")
    finally:
        sock.close()


if __name__ == "__main__":
    main()

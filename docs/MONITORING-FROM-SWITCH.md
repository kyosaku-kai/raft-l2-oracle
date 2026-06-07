# Monitoring Oracle Activity from the Switch

Guide for running the L2 oracle monitoring tools from a laptop/NUC connected to the same CRS326-24G-2S+ bridge as the STM32 Nucleo board(s).

## Network Topology

```
CRS326-24G-2S+ (192.168.88.1, bridge-attic, ether1-ether8)
├── Port 2: Nucleo-F207ZG (MAC 02:CA:FE:01:00:01)
├── Port N: Your monitoring laptop (any port on same bridge)
└── (future) Ports for additional Nucleo boards (T11 multi-node)
```

- Flat bridge, no VLAN filtering — broadcast floods to all ports
- Oracle uses raw Ethernet II frames (no IP stack needed)
- STM32 sends to broadcast (FF:FF:FF:FF:FF:FF) — any port on the bridge sees TX frames

## Prerequisites

On the monitoring machine:

```bash
# Python 3 + scapy (for frame_sniffer.py)
pip install scapy
# OR: apt install python3-scapy

# Root access (raw sockets require CAP_NET_RAW or sudo)
```

No special drivers needed — just a working Ethernet interface on the bridge.

## Quick Start

### 1. Identify your interface

```bash
ip link show          # find the interface connected to CRS326
# Likely: eth0, enp0s*, enx* (USB-Ethernet)
```

### 2. Clone/copy this repo (or just the tools/ directory)

```bash
git clone <repo-url>
cd raft-l2-oracle
# OR: just copy tools/ — the scripts are self-contained
```

### 3. Sniff oracle frames (passive monitoring)

```bash
sudo python3 tools/frame_sniffer.py <iface>

# Filter by type:
sudo python3 tools/frame_sniffer.py <iface> --type raft
sudo python3 tools/frame_sniffer.py <iface> --type heartbeat
sudo python3 tools/frame_sniffer.py <iface> --type health

# Save to pcap for Wireshark analysis:
sudo python3 tools/frame_sniffer.py <iface> --pcap capture.pcap -v
```

### 4. Send heartbeats (simulate a compute node)

```bash
# Default: node_id=101, box_id=1, 10ms interval
sudo python3 tools/heartbeat_sender.py <iface>

# Custom settings:
sudo python3 tools/heartbeat_sender.py <iface> --node-id 101 --box-id 1 --interval 50

# Test failure detection (send for 5s, then stop):
sudo python3 tools/heartbeat_sender.py <iface> --duration 5
```

### 5. Full automated verification (if you also have the ST-LINK)

```bash
./tools/verify_t10.sh full         # build, flash, verify
./tools/verify_t10.sh sniff eth0   # just sniff
./tools/verify_t10.sh heartbeat eth0  # just send heartbeats
```

## Wire Protocol Reference

| EtherType | Name      | Direction         | Purpose                                |
|-----------|-----------|-------------------|----------------------------------------|
| 0x88B5    | RAFT      | STM32 ↔ STM32     | Raft consensus (AppendEntries, votes)  |
| 0x88B6    | HEALTH    | STM32 → broadcast | Health state changes, failure events   |
| 0x88B7    | HEARTBEAT | Compute → STM32   | Node heartbeats, announce/ack          |

### Oracle Protocol Header (10 bytes, little-endian)

```
Offset  Size  Field
0       1     version (high nibble = major, low = minor; current: 0x10)
1       1     msg_type (see below)
2       1     node_id
3       1     box_id
4       4     term (raft term, LE uint32)
8       2     payload_len (LE int16)
```

### Message Types

| Code | Name               | EtherType | Notes                        |
|------|--------------------|-----------|------------------------------|
| 0x01 | REQUEST_VOTE       | 0x88B5    | Raft leader election         |
| 0x02 | REQUEST_VOTE_RESP  | 0x88B5    |                              |
| 0x03 | APPEND_ENTRIES     | 0x88B5    | Raft heartbeat/replication   |
| 0x04 | APPEND_ENTRIES_RESP| 0x88B5    | May carry health observations|
| 0x10 | HEALTH_UPDATE      | 0x88B6    | State transition event       |
| 0x11 | CLUSTER_STATE      | 0x88B6    | Full cluster snapshot        |
| 0x12 | FAILURE_EVENT      | 0x88B6    | Node declared DOWN           |
| 0x20 | NODE_HEARTBEAT     | 0x88B7    | Periodic liveness signal     |
| 0x21 | NODE_ANNOUNCE      | 0x88B7    | Compute node registration    |
| 0x22 | NODE_ANNOUNCE_ACK  | 0x88B7    | Oracle confirms registration |

## What to Expect

### Single-node (T10, current state)

- **No raft frames** (0x88B5) — single node has no peers to send to
- **Health frames** (0x88B6) only on state transitions (need heartbeat_sender running)
- **After sending heartbeats**: UART on STM32 shows `health: N hb, 1 nodes tracked`
- **After stopping heartbeats**: STM32 detects SUSPECT (~30ms), then DOWN (~80ms), emits FAILURE_EVENT on 0x88B6

### Multi-node (T11, upcoming)

- **Raft heartbeats** every 50ms from leader to followers (APPEND_ENTRIES with n_entries=0)
- **Leader election** visible as REQUEST_VOTE burst after leader disconnect
- **Health replication** via raft log — all nodes converge on same health table

## STM32 UART Output Reference

The monitoring stats line (every 5s) looks like:

```
[5005 ms] heap: 28840 free, 28840 min-ever
  hwm: rx=416 raft=964 health=400 mon=406
  queues: raft=0/16 hb=0/8
  raft: state=LEADER term=0 leader=1
  eth: tx=0 err=0 rx=0 drop=0 filt=6 dma=0
health: 0 hb, 0 nodes tracked
```

Key fields:
- `eth: tx/rx` — oracle frames sent/received (excludes filtered non-oracle traffic)
- `filt` — frames seen by MAC but filtered (wrong EtherType)
- `health: N hb, M nodes` — heartbeats received, nodes being monitored

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| Sniffer sees nothing | Wrong interface / not on bridge | Check `ip link`, ensure cable to CRS326 |
| STM32 shows `filt` increasing but `rx=0` | Traffic exists but no oracle frames | Expected if only normal LAN traffic |
| heartbeat_sender runs but STM32 `rx=0` | Frames not reaching STM32 | Check bridge config, same VLAN/bridge group |
| `Permission denied` on tools | Need root for raw sockets | Use `sudo` |
| STM32 UART shows no `eth:` line | ETH init failed (stub transport) | Power-cycle with cable connected |

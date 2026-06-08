# Hackathon Demo Runbook (2026-06-09)

## Quick Start: Simulator Demo (any machine)

```bash
nix develop
cd sim/build && cmake .. && make -j$(nproc)

# Basic: 3-node Raft consensus, leader election, health monitoring
./raft_sim 3

# Chaos: failure detection + leader kill + re-election
./raft_sim 3 --chaos
```

### What to narrate

| Time | Event | What's happening |
|------|-------|-----------------|
| t=0 | Nodes boot | 3 STM32 oracle nodes start, compute heartbeats begin |
| t=1s | Leader elected | One node wins election (term 1), heartbeats → all nodes mark compute UP |
| t=3s | **CHAOS: kill heartbeat** | Simulates compute node failure (NIC death, kernel panic) |
| t=3.03s | SUSPECT committed | Health monitor detects 3 missed heartbeats, proposes via Raft |
| t=3.08s | **DOWN committed** | All 3 nodes agree: `commit=2`, `n101=DOWN` — fencing trigger |
| t=8s | **CHAOS: kill leader** | Leader node dies (power loss, hardware fault) |
| t=9s | New leader elected | Surviving nodes elect new leader (term 2), health state preserved |

**Key demo point**: All nodes have identical health state (`commit=2, n101=DOWN`) — the DOWN decision is *consensus-backed*, not single-observer. Health state survives leader failure.

---

## Hardware Demo: T10 Verification

### Prerequisites
- Nucleo-F207ZG on CRS326 port 2 (ether2), flashed with `build/raft_oracle_node1.hex`
- A Linux machine with physical NIC on the same CRS326 bridge (any port, flat bridge)
- `scapy` installed on that machine (`pip install scapy` or `nix-shell -p python3Packages.scapy`)

### Step 1: Flash the board (from machine with USB to Nucleo)

```bash
openocd -f interface/stlink.cfg -f target/stm32f2x.cfg \
  -c "program build/raft_oracle_node1.hex verify reset exit"
```

### Step 2: Monitor UART (115200 baud, ST-LINK VCP)

```bash
# Find the VCP device
ls /dev/ttyACM* /dev/serial/by-id/*STLink*
# Monitor
picocom -b 115200 /dev/ttyACM0
```

Expected output:
```
[boot] raft-l2-oracle v0.8
[raft] state=LEADER term=1 (1-of-1 quorum)
[heap] used=73KB free=29KB hwm=29KB
[task] eth_rx: 1.2K  raft: 2.1K  health: 1.0K (stack HWM bytes)
```

### Step 3: Verify TX — sniff oracle frames (from laptop on CRS326)

```bash
# Find your interface on the CRS326 bridge
ip link show  # look for the physical NIC

# Run sniffer (needs root for raw socket + scapy)
sudo python3 tools/frame_sniffer.py <iface> --type raft
```

Expected: raft heartbeat frames (AppendEntries to broadcast, since no peers configured for node1-only):
```
14:23:01.123 [RAFT     ] node=1 box=1 term=   1 APPEND_ENTRIES       prev=0/0 commit=0 [heartbeat]
```

### Step 4: Verify RX — send heartbeats (from laptop on CRS326)

```bash
# Send heartbeats for 5s then stop (triggers SUSPECT/DOWN detection)
sudo python3 tools/heartbeat_sender.py <iface> --node-id 101 --duration 5
```

Expected UART output on Nucleo:
```
[health] node 101 registered (type=X86)
[health] node 101: UP
... (5 seconds of heartbeats) ...
[health] node 101: SUSPECT (missed 3)
[health] node 101: DOWN
```

### Step 5: Automated verification

```bash
./tools/verify_t10.sh full
```

---

## Hardware: Multi-Node (T11, if 3 boards available)

```bash
# Flash each board with its node-specific firmware
openocd -f interface/stlink.cfg -f target/stm32f2x.cfg \
  -c "program build/raft_oracle_node1.hex verify reset exit"
# (repeat for node2, node3 on separate boards)
```

Connect all 3 Nucleos to CRS326. They'll discover each other via broadcast frames and elect a leader within 500ms.

---

## Architecture Talking Points

- **Why L2?** Sub-millisecond failure detection. TCP/IP adds 10-100ms of kernel buffering. Raw Ethernet frames bypass the entire network stack.
- **Why Raft?** Consensus prevents split-brain. Single observer can't fence — needs majority agreement.
- **Why STM32?** Independent failure domain. If the compute node's kernel panics, the oracle still detects it and tells other nodes to fence.
- **Wire format**: Custom EtherTypes (0x88B5 Raft, 0x88B6 Health, 0x88B7 Heartbeat). 10-byte oracle header + payload. All little-endian, packed structs.
- **Memory budget**: 128KB SRAM total. 73KB used (FreeRTOS + raft log + health table). No dynamic allocation after boot.

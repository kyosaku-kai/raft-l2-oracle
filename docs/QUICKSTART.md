# Quick Start Guide

Get up and running in 5 minutes. For full details, see the [README](../README.md).

## 1. Enter the dev shell

```bash
cd ~/src/raft-l2-oracle
nix develop
```

This provides: `arm-none-eabi-gcc`, `cmake`, `ninja`, `openocd`, `python3` (with scapy), `picocom`.

## 2. Build and run the simulator

```bash
cd sim && mkdir -p build && cd build
cmake .. && make -j$(nproc)

./raft_sim 3            # 3-node consensus, 10s run
./raft_sim 3 --chaos    # failure injection demo, 15s run
```

The simulator uses the same oracle code as the firmware — only the transport layer differs (UDP loopback vs Ethernet DMA).

## 3. Build firmware (requires arm-none-eabi-gcc from nix develop)

```bash
# Build for a single node
cmake --preset firmware -DNODE_ID=1
cmake --build build/firmware

# Build all 3 nodes at once
./tools/build_all_nodes.sh
```

Output: `build/raft_oracle_node{1,2,3}.hex`

## 4. Flash to hardware (requires Nucleo USB connection)

```bash
openocd -f interface/stlink.cfg -f target/stm32f2x.cfg \
  -c "program build/raft_oracle_node1.hex verify reset exit"
```

## 5. Monitor UART (115200 baud)

```bash
picocom -b 115200 /dev/ttyACM0
```

## 6. Test Ethernet (requires machine on same L2 bridge as Nucleo)

```bash
# Sniff oracle frames
sudo python3 tools/frame_sniffer.py <interface>

# Send heartbeats (simulates compute node)
sudo python3 tools/heartbeat_sender.py <interface> --duration 5
```

---

## Project at a glance

```
What:    L2 failure oracle — consensus-backed node health monitoring
Where:   STM32 Nucleo-F207ZG (Cortex-M3, 128KB SRAM, Ethernet)
How:     Raft consensus over raw Ethernet frames (no IP stack)
Why:     Detect compute node failures in <100ms from independent hardware
```

**Key files to read first:**
1. `firmware/protocol/wire_format.h` — the wire protocol (all message types and structs)
2. `firmware/raft/raft_oracle.c` — the Raft integration layer
3. `firmware/health/health_monitor.c` — the health state machine
4. `sim/sim_main.c` — the simulator (shows how everything fits together)

**Diagrams (in `docs/diagrams/`):**
- `system-architecture.drawio.svg` — high-level 3-tier view
- `firmware-internals.drawio.svg` — FreeRTOS tasks and data flow
- `failure-detection.drawio.svg` — timing of failure detection
- `raft-message-flow.md` — Mermaid sequence diagrams of Raft exchanges
- `health-state-machine.md` — state machine and decision flow
- `network-topology.md` — physical network layout and MAC scheme

**Sister project:** `~/src/n3x-infrathon` — oracle-agent (host daemon) + Debian images. Shares `wire_format.h`.

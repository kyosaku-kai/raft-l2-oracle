# raft-l2-oracle

Consensus-backed failure detection for IFE high-availability clusters, running on physically isolated microcontrollers.

## The problem

In a distributed IFE system, each box contains compute nodes (Jetson, x86) running k3s with etcd. When a node crashes or a box loses power, k3s takes 40+ seconds to detect the failure and cannot automatically restore etcd quorum. The current software-based failure detection (Chassis Manager, Node Manager) runs on the same processors it monitors - when a Jetson kernel panics, its failure monitor dies with it.

## The idea

Each box in the IFE system contains an AC5P switching ASIC with an embedded Cortex-M3 service processor. This CM3 is on independent silicon from the compute nodes it monitors - separate voltage rail, separate firmware, separate failure domain. It survives Jetson crashes, OOM kills, kernel panics, and module-level hardware faults. It also sits directly on the L2 switch fabric with sub-microsecond frame visibility.

This project puts that idle CM3 to work: run a lightweight Raft consensus algorithm across the CM3s, have each CM3 monitor its local compute nodes via L2 heartbeats, and reach cluster-wide agreement on which nodes are alive or dead. The result is consensus-backed failure detection in under 100ms, with automatic etcd quorum restoration in under 2 seconds.

## Architecture

The system has three tiers:

```
                    +-----------+
                    |  k3s/etcd |  (unchanged - receives fencing commands)
                    +-----+-----+
                          |
                    +-----+-----+
                    |oracle-agent|  Tier 2: Linux daemon on each compute node
                    +-----+-----+  Sends heartbeats, executes fencing actions
                          |
                   0x88B7 | 0x88B6
                  (hbeat) | (health)
                          |
                    +-----+-----+
                    |   STM32   |  Tier 1: L2 Oracle (this project)
                    | Raft node |  Monitors local nodes, votes on failures
                    +-----+-----+
                          |
                   0x88B5 | (Raft)
                          |
              +-----------+-----------+
              |                       |
        +-----+-----+          +-----+-----+
        |   STM32   |          |   STM32   |
        | Raft node |          | Raft node |
        +-----------+          +-----------+
```

**Tier 1 - L2 Oracle (this repo):** 3-4 STM32 microcontrollers running Raft consensus over raw L2 Ethernet. Each monitors its local compute nodes via 10ms heartbeats and proposes state transitions (UP -> SUSPECT -> DOWN) to the Raft leader. All STM32s maintain an identical, consensus-backed health state table.

**Tier 2 - Oracle-Agent (host/):** A Linux daemon on each compute node. Sends L2 heartbeat frames to its local STM32 ("I'm alive"). Receives health events and executes fencing: `kubectl cordon`, `etcdctl member remove`, `kubectl uncordon`. Cross-checks etcd before destructive actions as a safety interlock.

**Tier 3 - k3s/etcd (unchanged):** Runs normally. The oracle is purely additive - it never replaces or participates in etcd consensus. k3s's built-in tombstone mechanism handles automatic member rejoin after removal.

### Wire protocol

All inter-component communication uses raw Ethernet II frames with custom EtherTypes:

| EtherType | Name | Direction | Purpose |
|-----------|------|-----------|---------|
| `0x88B5` | Raft | STM32 <-> STM32 | Consensus messages (RequestVote, AppendEntries) |
| `0x88B6` | Health | STM32 -> compute | Failure events, cluster state reports |
| `0x88B7` | Heartbeat | Compute -> STM32 | "I'm alive" + load metrics |

Frames use a 24-byte header (14-byte Ethernet II + 10-byte oracle protocol header) followed by 0-64 bytes of packed, little-endian payload. No IP stack, no ARP, no TCP - just L2. This matches the production CM3's communication model (management-frame inject/extract through the switch backplane) and eliminates an entire class of failure modes.

### Failure detection flow

A single node crash is detected and fenced in under 200ms:

```
t=0ms     x86-2 crashes. Heartbeats (0x88B7) stop arriving at local STM32-2.
t=30ms    STM32-2 sees 3 missed heartbeats, marks x86-2 SUSPECT.
t=40ms    Raft leader commits SUSPECT. oracle-agent runs: kubectl cordon x86-2
t=80ms    No recovery. STM32-2 escalates to DOWN.
t=90ms    Raft leader commits DOWN. oracle-agent cross-checks etcd.
t=120ms   oracle-agent runs: etcdctl member remove x86-2
          etcd quorum restored.
```

For full-box power loss, multi-observer corroboration prevents false positives from asymmetric link failures: at least two STM32s must independently agree a node is down before fencing proceeds.

## What exists today

This repo implements the **POSIX simulator** (design doc milestone v0.7). The simulator runs the same oracle integration code that will run on STM32 firmware, but with UDP loopback instead of raw Ethernet and pthreads instead of FreeRTOS tasks.

**What works:**
- Wire protocol definitions - all packed C structs with static size assertions (`firmware/protocol/wire_format.h`)
- Transport HAL abstraction with swappable backends (`firmware/transport/transport.h`)
- UDP loopback transport for the simulator (`firmware/transport/transport_sim.c`)
- Oracle integration layer - callback bridge between willemt/raft and the transport HAL, message serialization/dispatch, RAM-only persistence (`firmware/raft/raft_oracle.c`)
- 3-node POSIX simulator demonstrating leader election and heartbeat replication (`sim/sim_main.c`)

**What does not exist yet:**
- STM32 firmware (FreeRTOS tasks, Ethernet HAL, linker scripts, cross-compilation)
- Health monitoring state machine (SUSPECT/DOWN escalation, multi-observer corroboration)
- Oracle-agent host daemon
- k3s fencing integration

## Repo structure

```
raft-l2-oracle/
  firmware/
    protocol/wire_format.h        # L2 wire protocol (packed structs, EtherTypes, frame header)
    transport/transport.h          # Transport HAL interface (send/recv/now_ms)
    transport/transport_sim.c      # POSIX UDP loopback backend
    transport/transport_sim.h
    raft/raft_oracle.h             # Oracle node context + API
    raft/raft_oracle.c             # Raft callback bridge + message dispatch
    health/health_monitor.h        # Health state machine (stub)
    vendor/raft/                   # willemt/raft as git submodule (timblaktu fork)
  sim/
    CMakeLists.txt                 # Simulator build (raft as static lib + our code)
    sim_main.c                     # 3-node threaded simulator
  host/                            # oracle-agent (not yet implemented)
  tools/                           # frame sniffer, chaos injection (not yet implemented)
  flake.nix                        # Nix dev shell
```

## Building and running

Requires Nix with flakes enabled.

```bash
nix develop                          # enter dev shell (gcc, cmake, arm-none-eabi-gcc, valgrind, etc.)
cd sim && mkdir -p build && cd build
cmake .. && make                     # build the POSIX simulator
./raft_sim                           # run 3-node simulation (default)
./raft_sim 5                         # run with 5 nodes
```

The simulator runs for 10 seconds, printing per-node status every second:

```
=== raft-l2-oracle POSIX simulator ===
Nodes: 3, tick: 50ms, runtime: 10s

[node 1] started (port 5001)
[node 2] started (port 5002)
[node 3] started (port 5003)
[node 1] state=LEADER term=1 leader=1 commit_idx=0
[node 2] state=FOLLOWER term=0 leader=1 commit_idx=0
[node 3] state=FOLLOWER term=0 leader=1 commit_idx=0
```

## Hardware target

**Hackathon board:** STM32 Nucleo-F207ZG (Cortex-M3 @ 120MHz, 128KB SRAM, 1MB Flash, integrated Ethernet MAC + LAN8742A PHY). Chosen because it matches the AC5P CM3's ARM profile and forces the same memory discipline (128KB is tight, which is the point).

**Production target:** AC5P embedded Cortex-M3 service processor. Only the transport layer changes - STM32 Ethernet HAL becomes CPSS management-frame inject/extract.

## Dependencies

- [willemt/raft](https://github.com/willemt/raft) (BSD-2-Clause) - C Raft consensus library, ~2.2K LOC, zero dependencies, callback-driven. Vendored as git submodule from [timblaktu/raft](https://github.com/timblaktu/raft) fork (adds 118 tests, 3 memory leak fixes, nix dev shell).

## Design document

Full specification including memory budget analysis, FreeRTOS task layout, multi-observer corroboration logic, and oracle-agent fencing taxonomy: `~/src/k3s-ha/raft-stm32-l2-design.md` (v0.5).

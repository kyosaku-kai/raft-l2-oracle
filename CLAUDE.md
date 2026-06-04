# raft-l2-oracle

L2 failure detection oracle using Raft consensus on STM32 Nucleo-F207ZG (Cortex-M3) with FreeRTOS. Detects node failures via L2 Ethernet heartbeats and provides consensus-backed health state to k3s clusters.

## Architecture

- **firmware/** - STM32 firmware: FreeRTOS tasks running willemt/raft + health monitoring + L2 transport
- **sim/** - POSIX simulator: same raft integration code, UDP loopback transport, pthreads instead of FreeRTOS
- **host/** - oracle-agent: Linux daemon on compute nodes, sends heartbeats to STM32, receives health events, executes fencing
- **tools/** - Development utilities (frame sniffer, chaos injection, latency benchmarks)

## Design Doc

Full specification: `~/src/k3s-ha/raft-stm32-l2-design.md` (v0.5)

## Key Technical Details

- Wire protocol: raw Ethernet II frames with custom EtherTypes (0x88B5 Raft, 0x88B6 Health, 0x88B7 Heartbeat)
- All structs are `__attribute__((packed))`, little-endian (ARM native)
- willemt/raft library is vendored as git submodule at `firmware/vendor/raft/` (timblaktu/raft fork, nix branch)
- The raft library is callback-driven and single-threaded - one raft_server_t per FreeRTOS task
- Custom heap via `raft_set_heap_functions()` for FreeRTOS `pvPortMalloc`/`vPortFree`

## Build

```bash
nix develop                    # enter dev shell
cd sim && mkdir build && cd build
cmake .. && make               # build POSIX simulator
./raft_sim                     # run 3-node simulation
```

## STM32 Target

- Board: Nucleo-F207ZG (Cortex-M3 @ 120MHz, 128KB SRAM, 1MB Flash, Ethernet)
- Fallback: Nucleo-F429ZI (Cortex-M4F, 256KB SRAM, same Ethernet)

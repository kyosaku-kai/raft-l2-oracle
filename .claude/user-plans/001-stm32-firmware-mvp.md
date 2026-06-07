# Plan 001: STM32 Firmware MVP

**Goal**: Transform raft-l2-oracle from a POSIX simulator into a complete STM32 firmware project that achieves design doc milestones v0.8 (single STM32 running Raft with L2 TX/RX on wire) and v0.9 (multi-node consensus with health table replication). Build system must work from terminal-only workflow (nix develop + cmake + make/ninja + openocd) while also supporting colleagues who prefer graphical IDEs (STM32CubeIDE, CLion, VS Code + Cortex-Debug).

**Hardware**: STM32 Nucleo-F207ZG (Cortex-M3 @ 120MHz, 128KB SRAM, 1MB Flash, Ethernet MAC + LAN8742A PHY)

**Reference**: `docs/design.md` (v0.5)

**Branch**: `dev` (branched from main at b192e28)

---

## Progress

| Task | Status | Description |
|------|--------|-------------|
| T1   | TASK:COMPLETE | Research STM32 build environment options and best practices |
| T2   | TASK:COMPLETE | Research FreeRTOS integration approaches for STM32F207 |
| T3   | TASK:COMPLETE | Set up CMake cross-compilation with arm-none-eabi toolchain |
| T4   | TASK:COMPLETE | Integrate STM32 HAL and startup code (minimal, no CubeMX bloat) |
| T5   | TASK:COMPLETE | Integrate FreeRTOS |
| T6   | TASK:COMPLETE | Implement STM32 Ethernet transport backend |
| T7   | TASK:COMPLETE | Adapt raft library for bare-metal (heap functions, log pre-allocation) |
| T8   | TASK:COMPLETE | Wire up FreeRTOS tasks (eth_rx, raft, health, timer) |
| T9   | TASK:COMPLETE | Implement health monitoring state machine |
| T10  | TASK:IN_PROGRESS | Flash, boot, and verify single-node Raft on hardware (v0.8) |
| T10b | TASK:PENDING | T11 software prep: multi-node config, cluster state broadcast |
| T11  | TASK:PENDING | Multi-node consensus on hardware (v0.9) |
| T12  | TASK:COMPLETE | IDE support files and colleague onboarding documentation |

---

## Task Details

### T1: Research STM32 build environment options and best practices

**Goal**: Understand the landscape of STM32 development tooling and determine the right approach for this project. We need a terminal-first workflow that doesn't depend on STM32CubeIDE, but must also be compatible with graphical IDEs colleagues may prefer.

**Research questions (all must be answered with concrete findings, not opinions)**:

1. **STM32CubeMX vs. hand-written HAL initialization**:
   - What does CubeMX actually generate? (file list, structure, what's boilerplate vs. project-specific)
   - Can we use CubeMX to generate initial code once, then maintain by hand? What are the pitfalls?
   - What's the minimal HAL initialization for our use case (GPIO for LED, UART for debug, Ethernet MAC)?
   - How do projects like libopencm3, stm32-rs, or Zephyr handle this without CubeMX?

2. **Build systems**:
   - CMake with arm-none-eabi-gcc: how do mature STM32 projects set this up? (toolchain file, linker script integration, flash targets)
   - Does STM32CubeIDE use its own build system or can it import CMake projects?
   - What about Meson, Make, or Ninja as alternatives?
   - How do projects handle the "two build targets" problem (firmware + simulator from shared source)?

3. **Flashing and debugging from terminal**:
   - openocd vs. st-flash vs. probe-rs: which works best with Nucleo-F207ZG?
   - GDB workflow: openocd as GDB server + arm-none-eabi-gdb
   - Can we add `flash` and `debug` targets to CMake?

4. **IDE compatibility**:
   - STM32CubeIDE: what project files does it need? Can it coexist with CMake?
   - CLion: native CMake support - any STM32-specific setup needed?
   - VS Code + Cortex-Debug extension: what config files (.vscode/launch.json, settings.json)?
   - Can all three IDEs use the same CMake project without conflicting project files?

5. **Existing reference projects**:
   - Find 3-5 well-structured open-source STM32 projects that use CMake + FreeRTOS without CubeIDE dependency
   - Look in ~/src/ first for any existing STM32 projects or clones
   - Check repos like: stm32-cmake (github), FreeRTOS-Plus examples, Zephyr's STM32 board support
   - Note their directory structure, how they handle HAL, startup, linker scripts

**Where to look first**: ~/src/ for existing clones, then github for reference projects. Web search as last resort for blog posts / tutorials comparing approaches.

**DoD**: Written summary in docs/ or the plan file itself with concrete recommendations, file trees from reference projects, and a decision on our approach. No code yet.

---

### T2: Research FreeRTOS integration approaches for STM32F207

**Goal**: Determine exactly how to bring FreeRTOS into this project and configure it for our use case.

**Research questions**:

1. **FreeRTOS source integration**:
   - Git submodule (github.com/FreeRTOS/FreeRTOS-Kernel) vs. vendored copy vs. STM32Cube firmware package?
   - Which FreeRTOS port files are needed for Cortex-M3? (portable/GCC/ARM_CM3/)
   - What's the minimal file set? (tasks.c, queue.c, list.c, timers.c, port.c, heap_N.c - which heap?)

2. **FreeRTOSConfig.h for our use case**:
   - configTOTAL_HEAP_SIZE: how much of our 128KB to give FreeRTOS heap vs. static allocation?
   - configMINIMAL_STACK_SIZE vs. our task stack sizes (2KB, 4KB)
   - Timer resolution: configTICK_RATE_HZ - we need 1ms ticks for raft_periodic()
   - Which heap implementation? heap_4.c (coalescing free) vs. heap_1.c (alloc-only, never free)?
   - Static vs. dynamic allocation (configSUPPORT_STATIC_ALLOCATION)?

3. **Memory allocation strategy**:
   - Design doc says raft library needs custom heap via raft_set_heap_functions()
   - Options: (a) route raft through FreeRTOS pvPortMalloc, (b) separate static pool for raft, (c) allocate everything at init then never free
   - The design doc recommends bare_realloc() returning NULL to prevent log doubling - does this interact with FreeRTOS heap?

4. **Task-to-ISR communication**:
   - Ethernet MAC RX interrupt -> eth_rx_task: how do STM32 projects bridge DMA completion to FreeRTOS queues?
   - xQueueSendFromISR vs. task notifications vs. stream buffers
   - Zero-copy DMA patterns: can we avoid memcpy from DMA buffer to queue?

**DoD**: Written summary with recommended FreeRTOS integration approach, file list, FreeRTOSConfig.h skeleton, and memory allocation strategy decision.

---

### T3: Set up CMake cross-compilation with arm-none-eabi toolchain

**Goal**: Create a working CMake build that cross-compiles a minimal "blinky" binary for STM32F207ZG and can be flashed.

**Depends on**: T1 findings (build system approach)

**Deliverables**:
- `firmware/arm-none-eabi.cmake` - CMake toolchain file for Cortex-M3
- `firmware/CMakeLists.txt` - top-level firmware CMake (initially just blinky)
- `firmware/stm32f207zg.ld` - linker script (Flash at 0x08000000, SRAM at 0x20000000, 128KB)
- Top-level `CMakeLists.txt` unifying firmware + simulator builds
- CMake targets: `firmware` (cross-compile .elf/.bin), `sim` (host build), `flash` (openocd), `debug` (openocd + gdb)
- Verify: `cmake --preset firmware && ninja` produces a .elf, `ninja flash` writes it to the board

**Key decisions from T1 that feed into this**:
- Linker script: hand-written vs. extracted from CubeMX
- How firmware/ and sim/ share source files (firmware/raft/, firmware/transport/, firmware/protocol/)
- Nix flake updates if new dependencies are needed

**DoD**: `nix develop`, then `cmake --preset firmware && ninja` produces a valid .elf for STM32F207ZG. `ninja flash` writes it to a connected Nucleo board via openocd/st-link. LED blinks or UART prints "hello" to confirm execution.

---

### T4: Integrate STM32 HAL and startup code (minimal, no CubeMX bloat)

**Goal**: Get the minimal STM32 HAL infrastructure needed for our peripherals: system clock, GPIO (LED for status), UART (debug output), and Ethernet MAC.

**Depends on**: T3 (working cross-compilation)

**Deliverables**:
- `firmware/core/startup_stm32f207xx.s` - vector table + Reset_Handler (from CMSIS or CubeMX, stripped to essentials)
- `firmware/core/system_stm32f2xx.c` - SystemInit() clock configuration (HSE -> PLL -> 120MHz)
- `firmware/core/stm32f2xx_it.c` - interrupt handlers (SysTick, HardFault, Ethernet, UART)
- STM32F2xx HAL driver files (only the ones we need): `stm32f2xx_hal.c`, `stm32f2xx_hal_rcc.c`, `stm32f2xx_hal_gpio.c`, `stm32f2xx_hal_uart.c`, `stm32f2xx_hal_eth.c`, `stm32f2xx_hal_cortex.c`
- `firmware/core/stm32f2xx_hal_conf.h` - HAL configuration (enable only needed modules)
- CMSIS headers: `stm32f207xx.h`, `system_stm32f2xx.h`, `core_cm3.h`

**Source for HAL/CMSIS files**: STM32CubeF2 package (github.com/STMicroelectronics/STM32CubeF2) or individual HAL driver repos. These are BSD-licensed.

**Approach decision (from T1)**:
- Option A: Generate with CubeMX once, then strip to essentials and maintain by hand
- Option B: Pull HAL drivers directly from STM32CubeF2 repo, write SystemInit and startup manually
- Option C: Use a lightweight alternative like libopencm3

**DoD**: Firmware boots on Nucleo-F207ZG, configures 120MHz clock, toggles LED (LD1/LD2), prints "boot ok" over UART (USART3, routed to ST-LINK VCP on Nucleo-144 boards).

---

### T5: Integrate FreeRTOS

**Goal**: Add FreeRTOS kernel to the firmware build and verify basic task scheduling works.

**Depends on**: T2 findings (FreeRTOS approach), T4 (HAL + startup working)

**Deliverables**:
- FreeRTOS kernel source integrated (submodule or vendored, per T2 decision)
- `firmware/core/FreeRTOSConfig.h` - configured for our use case per T2 findings
- Heap implementation selected and configured
- SysTick routed to FreeRTOS tick handler (vPortSysTickHandler)
- Test: create 2 tasks that alternate toggling LEDs at different rates, proving preemptive scheduling works
- Test: create a queue, have one task send, another receive, proving IPC works

**Memory verification**: After boot, print FreeRTOS heap stats (xPortGetFreeHeapSize, xPortGetMinimumEverFreeHeapSize) over UART. Confirm total heap usage aligns with design doc budget (~16KB kernel+heap).

**DoD**: Two tasks running on FreeRTOS, communicating via queue, on real hardware. Heap usage reported and within budget.

---

### T6: Implement STM32 Ethernet transport backend

**Goal**: Create `firmware/transport/transport_eth.c` implementing the transport HAL interface over the STM32F207's Ethernet MAC + LAN8742A PHY. This is the real L2 transport that replaces `transport_sim.c` on hardware.

**Depends on**: T4 (HAL Ethernet driver), T5 (FreeRTOS for ISR-to-task bridging)

**Deliverables**:
- `firmware/transport/transport_eth.c` - implements `raft_transport_t` (send/recv/now_ms) using raw Ethernet frames
- Ethernet MAC initialization: MII/RMII mode (Nucleo-F207ZG uses RMII), DMA descriptors, MAC address from `02:CA:FE:<box>:00:01` scheme
- TX path: build 24-byte oracle frame header + payload, submit to DMA TX ring
- RX path: DMA interrupt -> deframe -> check EtherType -> post to appropriate FreeRTOS queue (raft_inbox or hb_inbox)
- PHY initialization (LAN8742A): auto-negotiation, link status monitoring
- `now_ms()`: return FreeRTOS xTaskGetTickCount() (1ms resolution)

**Wire verification**: Use a second device (laptop with tcpdump/scapy, or another STM32) to verify frames appear on wire with correct EtherType, MAC addresses, and payload format. The `tools/frame_sniffer.py` from the design doc would be valuable here.

**Key challenges**:
- STM32F2 Ethernet DMA descriptor setup is notoriously fiddly - get the HAL_ETH examples working first before customizing
- Raw frame TX/RX (not TCP/IP) requires bypassing LwIP - we want direct MAC access
- Cache/DMA coherency: F207ZG has no data cache (Cortex-M3), so this is simpler than F4/F7

**DoD**: STM32 sends a raw Ethernet frame (our oracle frame format) that is captured by tcpdump on a connected laptop. STM32 receives a raw frame sent by scapy from the laptop and prints its contents over UART.

---

### T7: Adapt raft library for bare-metal (heap functions, log pre-allocation)

**Goal**: Modify the vendored willemt/raft to work within our 128KB SRAM budget using the strategy from design doc Section 5.4.

**Depends on**: T5 (FreeRTOS heap available)

**Deliverables**:
- `raft_set_heap_functions()` called with our bare-metal allocators:
  - `bare_malloc` -> pvPortMalloc (or static pool, per T2 decision)
  - `bare_calloc` -> pvPortMalloc + memset
  - `bare_realloc` -> returns NULL (prevents log doubling)
  - `bare_free` -> vPortFree (or no-op if static pool)
- Patch `raft_log.c` to pre-allocate 1500 entries at init instead of starting at 10 and doubling (the one-line fix from design doc Section 5.4)
- `#define __log(...)` as no-op for release builds (eliminates 1KB stack buffer + ~50 format strings from .rodata)
- Verify: raft_new() + raft_set_callbacks() + raft_add_node() succeed on hardware
- Verify: memory usage after raft init matches design doc budget (~30KB for log metadata + 18KB for payloads)

**Testing approach**: Initialize raft on hardware, print heap stats before and after. The delta should be ~50KB (raft structs + log + node array). Run raft_periodic() in a loop for 60 seconds, verify no additional allocations occur (heap free stays constant).

**DoD**: Raft library initializes on STM32 within memory budget. No dynamic allocation after init (heap watermark stable). Release build omits debug logging.

---

### T8: Wire up FreeRTOS tasks (eth_rx, raft, health, timer)

**Goal**: Create the FreeRTOS task structure from design doc Section 6 and connect everything together.

**Depends on**: T5 (FreeRTOS), T6 (Ethernet transport), T7 (raft on bare-metal)

**Deliverables**:
- `firmware/app/main.c` - creates all tasks and queues, calls oracle_init()
- FreeRTOS tasks per design doc:
  - `eth_rx_task` (priority 4, 2KB stack): demux received frames by EtherType to raft_inbox (0x88B5) or hb_inbox (0x88B7)
  - `raft_task` (priority 3, 4KB stack): drain raft_inbox, call oracle_dispatch_raft_message(), call raft_periodic() via timer callback
  - `health_task` (priority 2, 2KB stack): drain hb_inbox, track heartbeat timeouts, propose state transitions
  - Timer: FreeRTOS software timer calling raft_periodic(raft, elapsed_ms) every ORACLE_TICK_MS (50ms per current sim, 1ms per design doc - decide which)
- FreeRTOS queues:
  - `raft_inbox` - 0x88B5 frames from eth_rx_task to raft_task
  - `hb_inbox` - 0x88B7 frames from eth_rx_task to health_task
  - `tx_queue` - outbound frames from raft_task/health_task to eth TX

**The existing oracle code (raft_oracle.c) should work largely unchanged** - the transport HAL abstraction means the same oracle_init/oracle_dispatch/oracle_destroy calls work on both sim and firmware. The main new work is the FreeRTOS task scaffolding around it.

**DoD**: All tasks created and running. UART prints task status (stack high-water marks, queue depths) every second. No stack overflows, no queue full events under normal operation.

---

### T9: Implement health monitoring state machine

**Goal**: Implement the health state machine from design doc Section 6 (health monitoring logic) and the multi-observer corroboration from Section 2 (failure detection flow).

**Depends on**: T8 (tasks wired up)

**Deliverables**:
- `firmware/health/health_monitor.c` - implements:
  - `node_health_entry_t` table (16 nodes max)
  - Heartbeat tracking: per-node last_seen_ms, miss counter
  - State transitions: UP -> SUSPECT (3 missed heartbeats, 30ms) -> DOWN (50ms no recovery)
  - On SUSPECT: piggyback observation on AppendEntries response (observation_entry_t in wire_format.h)
  - On DOWN: propose health transition to raft leader via raft_recv_entry()
- `firmware/health/health_table.c` - the replicated health state table updated by applylog callback
- Leader-side corroboration logic: `uint8_t corroboration[MAX_PEERS][MAX_NODES]` matrix, require K observers to agree before committing DOWN
- 0x88B6 health event emission: when state transitions commit, send HEALTH_UPDATE/FAILURE_EVENT to local compute node

**Testing**: Use the POSIX simulator to validate the state machine logic before deploying to hardware. Add chaos injection (kill/delay heartbeats) to sim_main.c.

**DoD**: On simulator: inject heartbeat failure for one node, observe SUSPECT in ~30ms, DOWN in ~80ms, with correct corroboration. On hardware: receive real 0x88B7 heartbeats from a laptop running a simple heartbeat sender script, stop the script, observe the STM32 detecting the failure.

---

### T10: Flash, boot, and verify single-node Raft on hardware (v0.8)

**Goal**: Design doc milestone v0.8 - single STM32 running Raft, L2 TX/RX proven on wire.

**Depends on**: T8 (all tasks wired up)

**Deliverables**:
- Single Nucleo-F207ZG running full firmware: FreeRTOS tasks, raft library, Ethernet transport
- Raft initializes as single node (becomes leader immediately since 1-of-1 quorum)
- UART debug output showing: raft state, term, heap usage, task stack watermarks
- Ethernet frames visible on wire (captured by frame_sniffer.py or tcpdump on connected laptop):
  - Outbound: raft heartbeats (AppendEntries to nonexistent peers - will fail to send, that's fine)
  - Inbound: process 0x88B7 heartbeat frames from laptop scapy script
- LED status: green = leader, red = no quorum (or similar visual indicator)

**Verification checklist**:
- [x] Board boots to FreeRTOS in <1 second
- [x] Raft state = LEADER (single node, trivial quorum)
- [x] Heap usage within design doc budget (<102KB total, >26KB free) — 29KB free at runtime
- [x] No stack overflows after 60 seconds continuous run — all HWMs healthy
- [ ] Ethernet TX produces valid oracle frames (verified by sniffer)
- [ ] Ethernet RX processes heartbeat frames (verified by UART log)

**Next steps for items 5-6** (Ethernet cable connected):
- Nucleo RJ45 connected to CRS326 port #2 (ether2). Laptop USB-C adapter on same bridge (TBD port).
- The STM32 sends broadcast frames (FF:FF:FF:FF:FF:FF) — flat bridge floods to all ports.
- Run `tools/frame_sniffer.py <iface>` on laptop/NUC to verify TX frames.
- Run `tools/heartbeat_sender.py <iface>` on laptop/NUC to test RX path.
- MikroTik port mirroring can be set up for persistent monitoring (useful for T11 multi-node).
- `tools/verify_t10.sh full` automates the UART analysis portion.

**Bugs fixed** (sessions 1-2, commits 1571f6c, d919f4d):
1. bare_realloc NULL — broke raft_add_node (node array growth)
2. NULL transport crash — ETH init fails with no cable, stub transport added
3. HAL_UART_Transmit HAL_GetTick dependency — replaced with register polling
4. SysTick clock source (ROOT CAUSE) — `configSYSTICK_CLOCK_HZ` define triggers wrong clock (AHB/8 = 15 MHz), 8x tick slowdown
5. setvbuf unbuffered stdout — nosys _sbrk breaks newlib-nano buffer alloc
6. ETH_IRQn left enabled after failed init — disabled to prevent interrupt loop
7. Heap-used calculation negative — heap_4 lazy init
8. Printf interleaving — offset health report by 2.5s

**Code quality fixes** (session 3, pre-hardware-test):
9. heartbeat_sender.py: `struct.pack("<BBBBIN")` → `"<BBBBIH"` — N is invalid with `<` prefix (would crash at runtime, blocking T10 item 6)
10. frame_sniffer.py: `"<BBBBIh"` → `"<BBBBIH"` — signed h for uint16 payload_len
11. Term propagation: added `uint32_t term` to transport send/recv interface; raft_oracle.c dispatch now uses wire term instead of local term (was using wrong term for all raft messages — critical for T11 multi-node)
12. Multi-frame RX: `eth_recv` now tries `HAL_ETH_GetReceivedFrame_IT` before blocking on semaphore, fixing frame loss when multiple frames arrive between task wakeups (binary semaphore can only store one signal)

**DoD**: Single STM32 running Raft with L2 frames on wire. All verification checklist items pass.

---

### T10b: T11 software prep — multi-node config, cluster state broadcast

**Goal**: Prepare all firmware code needed for multi-node operation so that when 3 boards are available, it's just flash-and-go. No hardware required for this task.

**Depends on**: T8 (task arch), T9 (health monitor)

**Deliverables**:

1. **Compile-time node configuration** (`firmware/config/node_config.h`)
   - `ORACLE_THIS_NODE_ID` and `ORACLE_THIS_BOX_ID` — per-board identity
   - Static peer table: `{ node_id, box_id, mac[6] }` for all cluster members
   - CMake `-DNODE_ID=N` flag to build per-board firmware variants
   - Default: 3-node cluster (IDs 1,2,3 all box_id=1) matching n3x-infrathon's kas overlays

2. **Multi-node boot in main.c**
   - Replace hardcoded `oracle_add_node(&g_oracle_ctx, 1, 1)` with loop over peer table
   - Add self + all peers from compile-time config
   - Keep single-node as a valid config (NUM_PEERS=0)

3. **MSG_CLUSTER_STATE periodic broadcast** (`health_table.c` or new `cluster_state.c`)
   - Every 1 second, broadcast 0x88B6 MSG_CLUSTER_STATE frame containing:
     - Leader node/box, quorum_healthy flag
     - Per-node health table dump (node_health_wire_t entries)
   - The oracle-agent in n3x-infrathon **depends on receiving this** for its safety interlock
   - Only leader sends (followers don't broadcast to avoid confusion)

4. **CMake multi-node build targets**
   - `make node1`, `make node2`, `make node3` — convenience targets
   - Or single parametric: `cmake -DNODE_ID=2 ..` → sets `ORACLE_THIS_NODE_ID=2`
   - Each produces `raft_oracle_nodeN.elf`

5. **Sim validation**
   - Verify 3-node sim still works after config refactor
   - Add sim_main.c cluster state display (print when MSG_CLUSTER_STATE would be sent)

**Identity alignment with n3x-infrathon**:
- STM32 node IDs: 1, 2, 3 (one per box in production; all box_id=1 for single-box hackathon demo)
- Compute node IDs (heartbeat senders): 101-104 (matching oracle-agent ORACLE_NODE_ID env vars)
- MAC scheme: `02:CA:FE:<box_id>:00:01` for STM32

**DoD**: `cmake -DNODE_ID=2 .. && make` produces a firmware that boots as node 2, adds nodes 1 and 3 as peers, and begins leader election. Sim 3-node test passes. MSG_CLUSTER_STATE logic ready for wire verification in T11.

---

### T11: Multi-node consensus on hardware (v0.9)

**Goal**: Design doc milestone v0.9 - 3 STM32s achieve Raft consensus, health table replicates, observation sideband flows.

**Depends on**: T10 (single node working), T9 (health state machine)

**Deliverables**:
- 3 Nucleo-F207ZG boards connected to a switch (or directly if only 2, via crossover)
- All 3 nodes discover each other (static configuration - node IDs and MAC addresses known at compile time for v1)
- Leader election completes within 500ms of all nodes booting
- AppendEntries heartbeats flowing at 50ms intervals (visible on sniffer)
- Health table replicated: all 3 nodes agree on cluster state
- Failure detection test: disconnect one board's Ethernet cable, observe SUSPECT -> DOWN transition on remaining nodes
- Observation sideband: follower observations piggybacked on AE responses, leader corroborates before committing DOWN

**Verification checklist**:
- [ ] 3 nodes elect a leader within 500ms
- [ ] Leader failure (unplug leader): new election within 500ms
- [ ] Heartbeat monitoring: simulated compute node heartbeat failure detected in <100ms
- [ ] Health state replicated to all nodes (UART dump matches across all 3)
- [ ] Corroboration: single-observer DOWN proposal requires confirmation from second observer
- [ ] 10-minute soak: zero false positives, zero state corruption

**DoD**: 3 STM32s running Raft consensus with health table replication. Failure detection demo works end-to-end on real hardware.

---

### T12: IDE support files and colleague onboarding documentation

**Goal**: Make it trivial for colleagues to clone, build, flash, and debug regardless of whether they use terminal or GUI IDE.

**Depends on**: T3-T10 (build system stable)

**Deliverables**:
- `.vscode/launch.json` - Cortex-Debug configuration for OpenOCD + ST-LINK
- `.vscode/settings.json` - C/C++ extension settings (include paths, defines for STM32F207xx)
- `.vscode/tasks.json` - build/flash/clean tasks
- `.vscode/c_cpp_properties.json` - IntelliSense configuration for arm-none-eabi
- `compile_commands.json` generation via CMake (works with CLion, VS Code, clangd)
- STM32CubeIDE `.project` / `.cproject` if feasible without conflicting with CMake (may be skip-worthy - investigate in T1)
- Updated README.md with:
  - Getting started for terminal users (nix develop, cmake, ninja, flash)
  - Getting started for VS Code users (extensions to install, open folder, F5 to debug)
  - Getting started for CLion users (open CMakeLists.txt, configure toolchain)
  - UART console setup (115200 baud, which USB port on Nucleo-144)
  - Hardware setup photos/diagram (which Nucleo pins, switch connections, power)
- `docs/architecture.md` - firmware architecture overview for new contributors (task diagram, data flow, where to add new features)

**DoD**: A colleague can clone the repo, enter nix develop (or install tools manually), build, flash, and see UART output within 15 minutes using either terminal or their preferred IDE. README tested by at least one other person.

---

## Key Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| STM32F2 Ethernet DMA setup is complex and fragile | High | Blocks T6, T10 | Start from working HAL_ETH example, not from scratch. Budget extra time for T6. |
| 128KB SRAM is tight with FreeRTOS + raft + buffers | Medium | Blocks T7/T8 | Monitor heap continuously. First lever: reduce log from 1500 to 500 entries (saves 32KB). |
| Raw Ethernet TX/RX without LwIP is underdocumented | Medium | Slows T6 | Many forum posts exist. STM32F2 HAL_ETH has raw mode. Worst case: use LwIP raw sockets as thin wrapper. |
| CubeMX-generated code hard to maintain by hand | Medium | Slows T4 | T1 research determines whether to use CubeMX at all. May go pure HAL driver approach. |
| Colleagues unfamiliar with Nix | Low | Slows adoption | Provide non-Nix setup instructions as fallback (apt install arm-none-eabi-gcc cmake openocd). |
| FreeRTOS + Ethernet IRQ priority conflicts | Medium | Subtle bugs | Use FreeRTOS interrupt nesting correctly (configMAX_SYSCALL_INTERRUPT_PRIORITY). Test under load. |

## Scope Boundary

This plan covers firmware milestones v0.8 and v0.9. It does NOT cover:
- Oracle-agent host daemon (v0.10)
- k3s fencing integration (v0.11)
- Demo / chaos testing (v1.0)
- Production SoC service processor transport (v2)
- Persistent storage on flash
- Dynamic membership changes
- Snapshot / log compaction

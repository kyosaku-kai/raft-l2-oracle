# T1: STM32 Build Environment Research Findings

Research completed 2026-06-04. Covers build system, HAL initialization, flashing/debugging, IDE compatibility, and reference projects for the STM32 Nucleo-F207ZG (Cortex-M3 @ 120MHz) target.

## Decisions Summary

| Question | Decision | Rationale |
|----------|----------|-----------|
| CubeMX vs hand-written HAL | Generate once, hand-maintain | Get validated clock tree + pin config, then own the code |
| Build system | CMake + hand-written toolchain file + CMake Presets | Transparent, no framework dependency, IDE-compatible |
| Flashing | OpenOCD (primary), st-flash (quick alternative) | Both already in flake.nix, OpenOCD has best FreeRTOS debug |
| Debugging | OpenOCD GDB server + arm-none-eabi-gdb | RTOS-aware, SVD peripheral view, Cortex-Debug compatible |
| IDE | VS Code + CMake Tools + Cortex-Debug | Free, terminal-friendly, excellent CMake/debug integration |
| FreeRTOS source | Git submodule (FreeRTOS-Kernel, native CMake) | First-class CMake support, pin to release tag |
| Two-target build | Separate build dirs + CMake Presets | Can't cross-compile and native-compile in one configure |
| STM32CubeIDE | Skip for primary dev, don't generate project files | Fights CMake, Eclipse-based, adds maintenance burden |

## 1. CubeMX vs Hand-Written HAL Initialization

### What CubeMX generates

- `Core/Src/main.c` with `SystemClock_Config()`, `MX_GPIO_Init()`, `MX_ETH_Init()`, `MX_USART3_Init()`
- `Core/Src/stm32f2xx_hal_msp.c` - low-level pin/clock setup per peripheral
- `Core/Src/stm32f2xx_it.c` - interrupt handlers
- `Core/Src/system_stm32f2xx.c` - SystemInit, clock startup
- `Core/Inc/stm32f2xx_hal_conf.h` - HAL module enable/disable
- `startup_stm32f207zgtx.s` - vector table + Reset_Handler
- `STM32F207ZGTx_FLASH.ld` - linker script
- CubeMX preserves user code between `/* USER CODE BEGIN */` / `/* USER CODE END */` pairs

### Why generate-once is the right call

- Clock tree config for F207 at 120MHz with Ethernet is non-trivial (PLL multipliers, bus prescalers) - CubeMX validates it
- Ethernet RMII pin mapping is error-prone by hand
- Startup assembly and linker script are correct for our specific chip variant
- We only configure hardware once (fixed Nucleo board), so regeneration isn't needed
- CubeMX is a Java GUI app - doesn't fit Nix reproducible builds

### Workflow

1. Create CubeMX .ioc for Nucleo-F207ZG
2. Configure: 120MHz clock from HSE, ETH RMII, USART3 (debug VCP), GPIO (LEDs)
3. Generate code (Makefile target - we ignore the build files)
4. Extract: `SystemClock_Config()`, startup `.s`, linker `.ld`, `hal_conf.h`, peripheral init
5. Drop into our `firmware/` tree, commit as baseline
6. Keep `.ioc` in repo for reference, never regenerate
7. If peripheral changes needed later: re-run CubeMX, diff output, cherry-pick changes

### Alternatives considered

- **libopencm3**: Lighter than ST HAL but less documentation, no CubeMX clock validation, smaller community for F2 series
- **STM32 LL drivers**: Lower-level than HAL, smaller code, but more manual work - consider for T6 Ethernet if HAL is too heavy
- **Zephyr**: Full RTOS+HAL replacement, way too heavy for this project's scope

## 2. Build System

### CMake toolchain file (hand-written, minimal)

```cmake
# cmake/arm-none-eabi-gcc.cmake
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)

set(CMAKE_EXECUTABLE_SUFFIX_C ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_ASM ".elf")

# Prevent test-compile (fails on bare-metal without linker script)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Cortex-M3 flags (no FPU)
set(CPU_FLAGS "-mcpu=cortex-m3 -mthumb")

set(CMAKE_C_FLAGS_INIT "${CPU_FLAGS} -ffunction-sections -fdata-sections")
set(CMAKE_ASM_FLAGS_INIT "${CPU_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${CPU_FLAGS} --specs=nosys.specs --specs=nano.specs -Wl,--gc-sections")
```

Key points:
- `CMAKE_SYSTEM_NAME Generic` triggers cross-compilation mode
- `CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY` avoids link-test failure
- `-ffunction-sections -fdata-sections` + `-Wl,--gc-sections` enables dead code elimination (critical for flash)
- `--specs=nosys.specs` provides bare-metal syscall stubs
- `--specs=nano.specs` uses smaller newlib-nano
- For F429ZI fallback: add `-mfloat-abi=hard -mfpu=fpv4-sp-d16`
- **NEVER use `-flto` with FreeRTOS** - breaks `vTaskDelay` and task scheduling

### CMake Presets for two-target builds

```json
{
    "version": 6,
    "configurePresets": [
        {
            "name": "sim",
            "displayName": "POSIX Simulator",
            "binaryDir": "${sourceDir}/build/sim",
            "generator": "Ninja"
        },
        {
            "name": "firmware",
            "displayName": "STM32F207 Firmware",
            "binaryDir": "${sourceDir}/build/firmware",
            "toolchainFile": "${sourceDir}/cmake/arm-none-eabi-gcc.cmake",
            "generator": "Ninja"
        }
    ],
    "buildPresets": [
        {"name": "sim", "configurePreset": "sim"},
        {"name": "firmware", "configurePreset": "firmware"}
    ]
}
```

Usage: `cmake --preset firmware && cmake --build --preset firmware`

### Why not ObKo/stm32-cmake

Evaluated the most popular community framework (1.4k stars). It provides auto-generated linker scripts, FindHAL/FindCMSIS/FindFreeRTOS modules, and a unified toolchain file. However:
- Adds abstraction that's opaque when debugging cross-compilation issues
- Depends on STM32Cube packages at specific paths
- Last tagged release (v2.1.0) is from 2021
- For single-target-family projects, a hand-written toolchain is simpler and more transparent
- FreeRTOS-Kernel's native CMake is strictly better than ObKo's FindFreeRTOS

## 3. Flashing and Debugging from Terminal

### OpenOCD (primary)

Already in `flake.nix`. Best overall option for Nucleo-F207ZG's ST-LINK/V2-1.

```bash
# Flash
openocd -f interface/stlink.cfg -f target/stm32f2x.cfg \
  -c "program build/firmware/app.elf verify reset exit"

# Debug (two terminals)
openocd -f interface/stlink.cfg -f target/stm32f2x.cfg          # GDB server on :3333
arm-none-eabi-gdb build/firmware/app.elf \
  -ex "target remote :3333" -ex "monitor reset halt" -ex "load"  # GDB client
```

Strengths: SWD/JTAG, SWO trace, RTOS-aware debugging (FreeRTOS thread listing), SVD peripheral register view, works with Cortex-Debug extension. Excellent STM32F2 support.

### st-flash (quick alternative)

Also in `flake.nix` (stlink package). Simpler for flash-only:

```bash
st-flash write build/firmware/app.bin 0x08000000
```

Limitations: ST-LINK only, no RTOS-aware debugging, less capable GDB server.

### probe-rs (future consideration)

Modern Rust-based tool. Single binary for flash + debug + RTT log capture. Not currently in flake.nix but available in nixpkgs. Better UX but STM32F2 is less battle-tested than F4/F7. Consider later if RTT logging is desired.

### CMake custom targets

Add to firmware/CMakeLists.txt:

```cmake
add_custom_target(flash
    COMMAND openocd -f interface/stlink.cfg -f target/stm32f2x.cfg
        -c "program ${CMAKE_BINARY_DIR}/app.elf verify reset exit"
    DEPENDS app.elf
)

add_custom_target(debug
    COMMAND openocd -f interface/stlink.cfg -f target/stm32f2x.cfg
)
```

## 4. IDE Compatibility

### VS Code (recommended primary)

Extensions needed:
- **CMake Tools** (ms-vscode.cmake-tools): handles configure/build/target via CMake Presets
- **Cortex-Debug** (marus25.cortex-debug): ARM debugging with OpenOCD backend

`.vscode/launch.json`:
```json
{
    "type": "cortex-debug",
    "request": "launch",
    "name": "Debug STM32",
    "servertype": "openocd",
    "cwd": "${workspaceFolder}",
    "executable": "${workspaceFolder}/build/firmware/app.elf",
    "configFiles": ["interface/stlink.cfg", "target/stm32f2x.cfg"],
    "svdFile": "${workspaceFolder}/firmware/STM32F207.svd",
    "preLaunchTask": "Build Firmware"
}
```

CMake Presets integrate with CMake Tools status bar (preset selector for firmware vs sim).

### CLion

Native CMake support - just open the project. Needs:
- Configure toolchain pointing to arm-none-eabi-gcc
- OpenOCD run configuration for flash/debug
- Reads CMakePresets.json natively

### STM32CubeIDE

Not recommended as development IDE. It:
- Uses its own Makefile-based build system (not CMake)
- Has experimental CMake import (since 1.14+) but community reports it's rough
- Is Eclipse-based (slow, dated)
- Fights external build systems

We will NOT generate .project/.cproject files. Colleagues who want STM32CubeIDE can use it for CubeMX peripheral configuration only, not for building.

### All three IDEs share

- `compile_commands.json` (generated by CMake with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`)
- `CMakePresets.json` for target selection
- OpenOCD for flashing/debugging

## 5. Reference Projects and Directory Structure

### Key reference projects

1. **Nathan Dumont's "Serious Business STM32 Development"** (2024) - Best match for our approach. STM32F429, CubeMX generate-once, FreeRTOS-Kernel as submodule with native CMake, disabled CubeMX's CMSIS-OS wrapper.

2. **FreeRTOS-Kernel official repo** - Has first-class CMake support. Integration via `add_subdirectory()` with `FREERTOS_PORT` and `FREERTOS_HEAP` variables. Port options: `GCC_ARM_CM3` (F207), `GCC_ARM_CM4F` (F429 fallback), `GCC_POSIX` (simulator).

3. **dimtass/stm32f103-cmake-template** (2024) - Cortex-M3 (same as F207). Toggleable FreeRTOS via CMake option. Warning: `-flto` breaks FreeRTOS completely.

### Recommended directory structure

```
raft-l2-oracle/
  CMakeLists.txt                     # Top-level: includes sim/ or firmware/ based on preset
  CMakePresets.json                   # firmware vs sim presets
  cmake/
    arm-none-eabi-gcc.cmake           # Cross-compilation toolchain file
  firmware/
    CMakeLists.txt                    # Firmware build (cross-compiled)
    cubemx/
      raft-l2-oracle.ioc             # CubeMX project (reference only)
    startup/
      startup_stm32f207zgtx.s        # Vector table + Reset_Handler
      system_stm32f2xx.c             # SystemInit, clock config
    linker/
      STM32F207ZGTx_FLASH.ld         # Linker script
    drivers/
      CMSIS/                         # CMSIS headers (from STM32CubeF2)
      STM32F2xx_HAL_Driver/          # HAL sources (only modules we use)
    config/
      FreeRTOSConfig.h               # Our FreeRTOS configuration
      stm32f2xx_hal_conf.h           # HAL module enables
    core/
      stm32f2xx_it.c                 # Interrupt handlers
      clock_config.c                 # SystemClock_Config()
      gpio_init.c                    # GPIO for LEDs
      uart_init.c                    # USART3 debug console
      eth_init.c                     # Ethernet MAC init
    app/                             # Application code (exists)
      main.c
    raft/                            # Raft integration (exists)
    health/                          # Health monitoring (exists)
    transport/                       # Transport backends (exists)
      transport.h                    # Abstract interface
      transport_stm32.c              # STM32 Ethernet (new)
      transport_sim.c                # UDP loopback (exists)
    protocol/                        # Wire format (exists)
    vendor/
      raft/                          # willemt/raft submodule (exists)
      freertos/                      # FreeRTOS-Kernel submodule (new)
  sim/
    CMakeLists.txt                   # Simulator build (native, exists)
    sim_main.c                       # POSIX simulator entry (exists)
  .vscode/
    launch.json                      # Cortex-Debug config
    settings.json                    # CMake Tools settings
    tasks.json                       # Build/flash tasks
    c_cpp_properties.json            # IntelliSense for arm-none-eabi
```

### Critical technical notes for F207ZG

- **HAL timebase**: Move to TIM6 or TIM7 so FreeRTOS owns SysTick cleanly. This is the #1 source of "random hardfaults" in STM32+FreeRTOS projects.
- **ISR priority**: Any ISR calling `...FromISR()` FreeRTOS APIs must have NVIC priority >= `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` (typically 5). Ethernet interrupt falls in this category.
- **No data cache**: F207 (Cortex-M3) has no D-cache, so DMA coherency is simpler than F4/F7.
- **Never `-flto`**: Link-time optimization breaks FreeRTOS task scheduling on GCC.
- **F429ZI fallback**: Same Ethernet peripheral, just needs `GCC_ARM_CM4F` port and FPU flags.

## 6. FreeRTOS Integration Approach (preview for T2)

**Source**: Git submodule of `FreeRTOS/FreeRTOS-Kernel` at a release tag (V11.1.0), placed at `firmware/vendor/freertos/`.

**CMake integration** (native, no wrapper needed):
```cmake
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/config)

set(FREERTOS_HEAP "4" CACHE STRING "" FORCE)
set(FREERTOS_PORT "GCC_ARM_CM3" CACHE STRING "" FORCE)
add_subdirectory(vendor/freertos)
```

**Heap**: heap_4.c (coalescing free) - needed because raft library does malloc/free.

**Config**: Project-specific `FreeRTOSConfig.h` in `firmware/config/`, not inside vendor dir.

This will be fully specified in T2.

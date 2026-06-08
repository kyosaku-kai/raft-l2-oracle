set(CMAKE_C_STANDARD 99)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Linker script
set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/linker/STM32F207ZGTx_FLASH.ld)

# Node identity: cmake -DNODE_ID=2 .. (default: 1)
if(NOT DEFINED NODE_ID)
    set(NODE_ID 1)
endif()

# Cluster size: cmake -DCLUSTER_NODES=1 for single-board testing (default: 3)
if(NOT DEFINED CLUSTER_NODES)
    set(CLUSTER_NODES 3)
endif()

# STM32F2 defines
add_compile_definitions(
    STM32F207xx
    USE_HAL_DRIVER
    ORACLE_THIS_NODE_ID=${NODE_ID}
    ORACLE_THIS_BOX_ID=1
    ORACLE_NUM_CLUSTER_NODES=${CLUSTER_NODES}
)

# --- FreeRTOS-Kernel (native CMake integration) ---

# Config target: tells FreeRTOS where to find FreeRTOSConfig.h
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/config
)

# Select port and heap before add_subdirectory
set(FREERTOS_HEAP "4" CACHE STRING "" FORCE)
set(FREERTOS_PORT "GCC_ARM_CM3" CACHE STRING "" FORCE)

add_subdirectory(vendor/freertos)

# --- HAL driver sources ---
set(HAL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/drivers/STM32F2xx_HAL_Driver/Src)
set(HAL_SOURCES
    ${HAL_SRC}/stm32f2xx_hal.c
    ${HAL_SRC}/stm32f2xx_hal_rcc.c
    ${HAL_SRC}/stm32f2xx_hal_rcc_ex.c
    ${HAL_SRC}/stm32f2xx_hal_gpio.c
    ${HAL_SRC}/stm32f2xx_hal_cortex.c
    ${HAL_SRC}/stm32f2xx_hal_uart.c
    ${HAL_SRC}/stm32f2xx_hal_eth.c
    ${HAL_SRC}/stm32f2xx_hal_dma.c
    ${HAL_SRC}/stm32f2xx_hal_pwr.c
    ${HAL_SRC}/stm32f2xx_hal_pwr_ex.c
    ${HAL_SRC}/stm32f2xx_hal_flash.c
    ${HAL_SRC}/stm32f2xx_hal_flash_ex.c
    ${HAL_SRC}/stm32f2xx_hal_tim.c
    ${HAL_SRC}/stm32f2xx_hal_tim_ex.c
)

# --- Raft library (vendored submodule) ---
add_library(raft_lib STATIC
    vendor/raft/src/raft_server.c
    vendor/raft/src/raft_server_properties.c
    vendor/raft/src/raft_node.c
    vendor/raft/src/raft_log.c
)
target_include_directories(raft_lib PUBLIC vendor/raft/include)
target_compile_options(raft_lib PRIVATE -Wall -Wextra -Wno-unused-parameter)
# Pre-allocate log to full capacity; disable debug logging for release builds
target_compile_definitions(raft_lib PRIVATE
    RAFT_LOG_INITIAL_CAPACITY=1500
    RAFT_NO_DEBUG_LOG
)

# Firmware executable — output name includes node ID
set(APP_NAME "raft_oracle_node${NODE_ID}")
add_executable(app
    startup/startup_stm32f207zg.s
    core/system_stm32f2xx.c
    core/stm32f2xx_it.c
    core/stm32f2xx_hal_timebase_tim.c
    core/syscalls.c
    app/main.c
    raft/raft_oracle.c
    raft/raft_heap.c
    transport/transport_eth.c
    health/health_monitor.c
    health/health_table.c
    ${HAL_SOURCES}
)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/CMSIS/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/CMSIS/Device/ST/STM32F2xx/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/STM32F2xx_HAL_Driver/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/raft
    ${CMAKE_CURRENT_SOURCE_DIR}/transport
    ${CMAKE_CURRENT_SOURCE_DIR}/protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/health
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/raft/include
)

target_link_libraries(app PRIVATE freertos_kernel raft_lib)

set_target_properties(app PROPERTIES OUTPUT_NAME ${APP_NAME})

target_link_options(app PRIVATE
    -T${LINKER_SCRIPT}
    -Wl,--print-memory-usage
)

# Ensure re-link when linker script changes
set_target_properties(app PROPERTIES
    LINK_DEPENDS ${LINKER_SCRIPT}
)

# Generate .bin and .hex from .elf
add_custom_command(TARGET app POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:app> $<TARGET_FILE_DIR:app>/${APP_NAME}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:app> $<TARGET_FILE_DIR:app>/${APP_NAME}.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:app>
    COMMENT "Generating ${APP_NAME}.bin and .hex, printing size"
)

# Flash via OpenOCD
add_custom_target(flash
    COMMAND openocd
        -f interface/stlink.cfg
        -f target/stm32f2x.cfg
        -c "program $<TARGET_FILE:app> verify reset exit"
    DEPENDS app
    COMMENT "Flashing via OpenOCD + ST-LINK"
)

# Start OpenOCD GDB server (connect with arm-none-eabi-gdb on :3333)
add_custom_target(debug
    COMMAND openocd
        -f interface/stlink.cfg
        -f target/stm32f2x.cfg
    COMMENT "Starting OpenOCD GDB server on :3333"
)

# Multi-node build:
#   mkdir build-node2 && cd build-node2
#   cmake -DNODE_ID=2 .. && make
# Or use the top-level Makefile convenience targets (if added).

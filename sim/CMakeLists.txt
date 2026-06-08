cmake_minimum_required(VERSION 3.16)

# Support both standalone (cd sim && cmake .) and subdirectory (cmake --preset sim) usage
if(NOT CMAKE_PROJECT_NAME OR CMAKE_PROJECT_NAME STREQUAL "raft_sim")
    project(raft_sim C)
endif()

set(CMAKE_C_STANDARD 99)

# Resolve firmware/ root relative to this file (works standalone or as subdirectory)
set(FIRMWARE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../firmware)
set(RAFT_DIR ${FIRMWARE_DIR}/vendor/raft)

# Raft library sources (from vendored submodule)
# Built with relaxed warnings since upstream doesn't target -Werror on GCC 15
add_library(raft_lib STATIC
    ${RAFT_DIR}/src/raft_server.c
    ${RAFT_DIR}/src/raft_server_properties.c
    ${RAFT_DIR}/src/raft_node.c
    ${RAFT_DIR}/src/raft_log.c
)
target_include_directories(raft_lib PUBLIC ${RAFT_DIR}/include)
target_compile_definitions(raft_lib PRIVATE RAFT_LOG_INITIAL_CAPACITY=1500)
target_compile_options(raft_lib PRIVATE -Wall -Wextra -g -O2 -Wno-unused-parameter)

# Oracle integration + simulator (our code, strict warnings)
add_executable(raft_sim
    ${CMAKE_CURRENT_SOURCE_DIR}/sim_main.c
    ${FIRMWARE_DIR}/raft/raft_oracle.c
    ${FIRMWARE_DIR}/raft/raft_heap.c
    ${FIRMWARE_DIR}/transport/transport_sim.c
    ${FIRMWARE_DIR}/health/health_monitor.c
    ${FIRMWARE_DIR}/health/health_table.c
)

target_include_directories(raft_sim PRIVATE
    ${RAFT_DIR}/include
    ${FIRMWARE_DIR}
    ${FIRMWARE_DIR}/raft
    ${FIRMWARE_DIR}/health
)
target_compile_definitions(raft_sim PRIVATE RAFT_SIM)
target_compile_options(raft_sim PRIVATE -Wall -Wextra -Werror -g -O2)
target_link_libraries(raft_sim raft_lib pthread)

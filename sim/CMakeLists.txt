cmake_minimum_required(VERSION 3.16)
project(raft_sim C)

set(CMAKE_C_STANDARD 99)

# Raft library sources (from vendored submodule)
# Built with relaxed warnings since upstream doesn't target -Werror on GCC 15
set(RAFT_DIR ${CMAKE_SOURCE_DIR}/../firmware/vendor/raft)

add_library(raft_lib STATIC
    ${RAFT_DIR}/src/raft_server.c
    ${RAFT_DIR}/src/raft_server_properties.c
    ${RAFT_DIR}/src/raft_node.c
    ${RAFT_DIR}/src/raft_log.c
)
target_include_directories(raft_lib PUBLIC ${RAFT_DIR}/include)
target_compile_options(raft_lib PRIVATE -Wall -Wextra -g -O2 -Wno-unused-parameter)

# Oracle integration + simulator (our code, strict warnings)
add_executable(raft_sim
    sim_main.c
    ${CMAKE_SOURCE_DIR}/../firmware/raft/raft_oracle.c
    ${CMAKE_SOURCE_DIR}/../firmware/transport/transport_sim.c
)

target_include_directories(raft_sim PRIVATE
    ${RAFT_DIR}/include
    ${CMAKE_SOURCE_DIR}/../firmware
)
target_compile_options(raft_sim PRIVATE -Wall -Wextra -Werror -g -O2)
target_link_libraries(raft_sim raft_lib pthread)

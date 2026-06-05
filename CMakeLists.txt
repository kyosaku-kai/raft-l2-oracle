cmake_minimum_required(VERSION 3.16)

# Top-level CMake: dispatches to firmware/ or sim/ based on preset.
# Never configured directly - always use: cmake --preset firmware  or  cmake --preset sim
#
# CMAKE_TOOLCHAIN_FILE is set before project() by the preset; use it to
# determine which subdirectory to include.

if(CMAKE_TOOLCHAIN_FILE)
    project(raft_l2_oracle C ASM)
    add_subdirectory(firmware)
else()
    project(raft_l2_oracle C)
    add_subdirectory(sim)
endif()

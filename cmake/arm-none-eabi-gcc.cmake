# CMake toolchain file for ARM Cortex-M3 (STM32F207ZG)
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
# Use find_program so CMake resolves these from the same prefix as the compiler
find_program(CMAKE_OBJCOPY arm-none-eabi-objcopy)
find_program(CMAKE_OBJDUMP arm-none-eabi-objdump)
find_program(CMAKE_SIZE arm-none-eabi-size)

set(CMAKE_EXECUTABLE_SUFFIX_C ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_ASM ".elf")

# Prevent test-compile (fails on bare-metal without linker script)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Cortex-M3 flags (no FPU)
set(CPU_FLAGS "-mcpu=cortex-m3 -mthumb")

set(CMAKE_C_FLAGS_INIT "${CPU_FLAGS} -ffunction-sections -fdata-sections -Wall -Wextra")
set(CMAKE_ASM_FLAGS_INIT "${CPU_FLAGS} -x assembler-with-cpp")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${CPU_FLAGS} --specs=nosys.specs --specs=nano.specs -Wl,--gc-sections -Wl,-Map=output.map")

set(CMAKE_C_FLAGS_DEBUG "-Og -g3 -DDEBUG" CACHE STRING "")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "")
set(CMAKE_C_FLAGS_MINSIZEREL "-Os -DNDEBUG" CACHE STRING "")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

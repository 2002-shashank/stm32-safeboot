cmake_minimum_required(VERSION 3.20)

# Host build (default):   cmake -B build/host -G Ninja && ninja -C build/host && ctest --test-dir build/host
# Target build:           cmake -B build/target -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
project(stm32_safeboot C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(SB_CORE_SOURCES
    core/src/boot.c
    core/src/crash.c
    core/src/crc32.c
    core/src/frame.c
    core/src/image.c
    core/src/journal.c
    core/src/proto.c
    core/src/sha256.c
)
set(SB_MONOCYPHER_SOURCES
    third_party/monocypher/monocypher.c
    third_party/monocypher/monocypher-ed25519.c
)

if(CMAKE_CROSSCOMPILING)
    add_subdirectory(target)
    return()
endif()

# ---------------- host ----------------
enable_testing()
set(SB_HOST_WARNINGS -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion)
option(SB_SANITIZE "Build host code with ASan/UBSan" ON)

add_library(monocypher STATIC ${SB_MONOCYPHER_SOURCES})
target_include_directories(monocypher PUBLIC third_party/monocypher)
target_compile_options(monocypher PRIVATE -O2)

add_library(sbcore STATIC ${SB_CORE_SOURCES} host/flash_sim.c host/testutil.c)
target_include_directories(sbcore PUBLIC core/include host)
target_link_libraries(sbcore PUBLIC monocypher)
target_compile_options(sbcore PRIVATE ${SB_HOST_WARNINGS})

if(SB_SANITIZE)
    target_compile_options(sbcore PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(sbcore PUBLIC -fsanitize=address,undefined)
endif()

add_executable(test_core host/test_core.c)
target_link_libraries(test_core sbcore)
target_compile_options(test_core PRIVATE ${SB_HOST_WARNINGS})
add_test(NAME test_core COMMAND test_core)

add_executable(fault_harness host/fault_harness.c)
target_link_libraries(fault_harness sbcore)
# Scenario tables use positional initializers with trailing fields left 0.
target_compile_options(fault_harness PRIVATE ${SB_HOST_WARNINGS} -Wno-missing-field-initializers)
add_test(NAME fault_harness_quick COMMAND fault_harness --quick)
# ~1 min in a release build, ~11 min under ASan: skip with `ctest -LE slow`.
set_tests_properties(fault_harness_quick PROPERTIES LABELS slow)

add_executable(sbhost host/sbhost.c)
target_link_libraries(sbhost sbcore)
target_compile_options(sbhost PRIVATE ${SB_HOST_WARNINGS})
set(SB_PYTHON ${PROJECT_SOURCE_DIR}/.venv/bin/python CACHE FILEPATH "Python with 'cryptography'")
add_test(NAME tools_cross_language
         COMMAND ${SB_PYTHON} ${PROJECT_SOURCE_DIR}/tools/test_tools.py $<TARGET_FILE:sbhost>)

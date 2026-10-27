# Target firmware (cross build). Included from the top level when
# CMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake.
set(SB_CMSIS_INCLUDES
    ${PROJECT_SOURCE_DIR}/third_party/cmsis/core
    ${PROJECT_SOURCE_DIR}/third_party/cmsis/device
)
set(SB_TARGET_WARNINGS -Wall -Wextra -Werror)

function(sb_firmware_outputs name)
    set_target_properties(${name} PROPERTIES SUFFIX ".elf")
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${name}> ${name}.bin
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${name}>
        BYPRODUCTS ${name}.bin
        VERBATIM)
endfunction()

# Phase 0: register-level blink + VCP wiring probe + DWT check.
add_executable(feasibility_blink feasibility/blink.c)
target_include_directories(feasibility_blink PRIVATE ${SB_CMSIS_INCLUDES})
target_compile_definitions(feasibility_blink PRIVATE STM32F407xx)
target_compile_options(feasibility_blink PRIVATE -Os -g ${SB_TARGET_WARNINGS})
target_link_options(feasibility_blink PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/feasibility/blink.ld -Wl,-Map=feasibility_blink.map)
sb_firmware_outputs(feasibility_blink)

# ---------------- bootloader ----------------
set(SB_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/common)
set(SB_CORE_INCLUDE ${PROJECT_SOURCE_DIR}/core/include)
list(TRANSFORM SB_CORE_SOURCES PREPEND ${PROJECT_SOURCE_DIR}/)
list(TRANSFORM SB_MONOCYPHER_SOURCES PREPEND ${PROJECT_SOURCE_DIR}/)

add_library(sb_target_core STATIC ${SB_CORE_SOURCES} ${SB_MONOCYPHER_SOURCES})
target_include_directories(sb_target_core PUBLIC ${SB_CORE_INCLUDE} ${PROJECT_SOURCE_DIR}/third_party/monocypher)
target_compile_options(sb_target_core PRIVATE -Os -g -fstack-usage)
set_source_files_properties(${SB_CORE_SOURCES} PROPERTIES COMPILE_OPTIONS "${SB_TARGET_WARNINGS}")

add_executable(bootloader
    bootloader/main.c
    bootloader/api.c
    common/fault.c
    common/startup.c
    common/board.c
    common/flash_f4.c
    common/rtt.c
)
target_include_directories(bootloader PRIVATE ${SB_COMMON_DIR} ${SB_CMSIS_INCLUDES})
target_compile_definitions(bootloader PRIVATE STM32F407xx SB_STARTUP_RESET_CYCCNT)
target_compile_options(bootloader PRIVATE -Os -g -fstack-usage ${SB_TARGET_WARNINGS})
target_link_libraries(bootloader PRIVATE sb_target_core)
target_link_options(bootloader PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/bootloader/bootloader.ld -Wl,-Map=bootloader.map -Wl,--print-memory-usage)
set_target_properties(bootloader PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bootloader/bootloader.ld)
sb_firmware_outputs(bootloader)

# ---------------- app images ----------------
# sb_app(<name> <slot A|B> <version> <behavior>) builds <name>.elf/.bin and,
# if SB_SIGNING_KEY exists, the signed <name>.sbimg.
set(SB_SIGNING_KEY ${PROJECT_SOURCE_DIR}/keys/dev_ed25519.pem CACHE FILEPATH "Ed25519 signing key (PEM)")
set(SB_PYTHON ${PROJECT_SOURCE_DIR}/.venv/bin/python CACHE FILEPATH "Python with 'cryptography'")

# Optional 5th argument: bytes of incompressible filler, to build near
# maximum-size images for worst-case boot timing.
function(sb_app name slot version behavior)
    if(slot STREQUAL "A")
        set(origin 0x08020200)
    else()
        set(origin 0x08080200)
    endif()
    add_executable(${name} app/main.c common/startup.c common/board.c common/rtt.c common/fault.c
        ${PROJECT_SOURCE_DIR}/core/src/crc32.c ${PROJECT_SOURCE_DIR}/core/src/crash.c)
    target_include_directories(${name} PRIVATE ${SB_COMMON_DIR} ${SB_CMSIS_INCLUDES} ${SB_CORE_INCLUDE})
    target_compile_definitions(${name} PRIVATE STM32F407xx APP_VERSION=${version} APP_BEHAVIOR=${behavior})
    if(ARGC GREATER 4)
        target_compile_definitions(${name} PRIVATE APP_FILLER_BYTES=${ARGV4})
    endif()
    target_compile_options(${name} PRIVATE -Os -g ${SB_TARGET_WARNINGS})
    target_link_options(${name} PRIVATE -T${CMAKE_CURRENT_SOURCE_DIR}/app/app.ld
        -Wl,--defsym=SB_APP_ORIGIN=${origin} -Wl,-Map=${name}.map)
    set_target_properties(${name} PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/app/app.ld)
    sb_firmware_outputs(${name})
    if(EXISTS ${SB_SIGNING_KEY})
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${SB_PYTHON} ${PROJECT_SOURCE_DIR}/tools/sbtool.py sign --key ${SB_SIGNING_KEY}
                    --slot ${slot} --version ${version} --in ${name}.bin --out ${name}.sbimg
            VERBATIM)
    endif()
endfunction()

sb_app(app_a_v1 A 1 0)
sb_app(app_b_v2 B 2 0)
sb_app(app_a_v3 A 3 0)
sb_app(app_b_v3_crash B 3 1)
sb_app(app_b_v3_hang B 3 2)
sb_app(app_a_max A 1 0 386000)

# ---------------- bench (Phase 0 measurements) ----------------
add_executable(bench bench/bench.c common/startup.c common/board.c common/flash_f4.c common/rtt.c)
target_include_directories(bench PRIVATE ${SB_COMMON_DIR} ${SB_CMSIS_INCLUDES})
target_compile_definitions(bench PRIVATE STM32F407xx SB_STARTUP_RESET_CYCCNT)
target_compile_options(bench PRIVATE -Os -g ${SB_TARGET_WARNINGS})
target_link_libraries(bench PRIVATE sb_target_core)
target_link_options(bench PRIVATE -T${CMAKE_CURRENT_SOURCE_DIR}/bootloader/bootloader.ld -Wl,-Map=bench.map)
sb_firmware_outputs(bench)

# ---------------- bootloader with hardware fault injection ----------------
add_executable(bootloader_fi
    bootloader/main.c bootloader/api.c common/fault.c common/startup.c common/board.c common/flash_f4.c
    common/rtt.c)
target_include_directories(bootloader_fi PRIVATE ${SB_COMMON_DIR} ${SB_CMSIS_INCLUDES})
target_compile_definitions(bootloader_fi PRIVATE STM32F407xx SB_STARTUP_RESET_CYCCNT SB_FAULT_INJECT)
target_compile_options(bootloader_fi PRIVATE -Os -g ${SB_TARGET_WARNINGS})
target_link_libraries(bootloader_fi PRIVATE sb_target_core)
target_link_options(bootloader_fi PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/bootloader/bootloader.ld -Wl,-Map=bootloader_fi.map)
sb_firmware_outputs(bootloader_fi)

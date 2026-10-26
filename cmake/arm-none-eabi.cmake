# Toolchain file for the Arm GNU Toolchain (arm-none-eabi).
# Set ARM_TOOLCHAIN_DIR if the toolchain is not at the default location.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT ARM_TOOLCHAIN_DIR)
    if(DEFINED ENV{ARM_TOOLCHAIN_DIR})
        set(ARM_TOOLCHAIN_DIR $ENV{ARM_TOOLCHAIN_DIR})
    else()
        set(ARM_TOOLCHAIN_DIR $ENV{HOME}/opt/arm-gnu-toolchain-15.3.rel2)
    endif()
endif()
set(ARM_TOOLCHAIN_DIR ${ARM_TOOLCHAIN_DIR} CACHE PATH "Arm GNU Toolchain root")

set(CMAKE_C_COMPILER ${ARM_TOOLCHAIN_DIR}/bin/arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER ${ARM_TOOLCHAIN_DIR}/bin/arm-none-eabi-gcc)
set(CMAKE_OBJCOPY ${ARM_TOOLCHAIN_DIR}/bin/arm-none-eabi-objcopy CACHE FILEPATH "")
set(CMAKE_SIZE ${ARM_TOOLCHAIN_DIR}/bin/arm-none-eabi-size CACHE FILEPATH "")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-m4 -mthumb -mfloat-abi=soft -ffunction-sections -fdata-sections")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostartfiles -Wl,--gc-sections --specs=nano.specs --specs=nosys.specs")

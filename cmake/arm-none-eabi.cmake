# Cross-compilation toolchain for the Electra One Mini (Renesas S7G2, Cortex-M4F).

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Skip CMake's compiler test: it links a hosted executable, which cannot work here.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(WIN32)
    set(TOOL_SUFFIX ".exe")
else()
    set(TOOL_SUFFIX "")
endif()

find_program(ARM_GCC arm-none-eabi-gcc
    HINTS "C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.2 rel1/bin"
          "C:/Program Files/Arm GNU Toolchain arm-none-eabi/14.2 rel1/bin"
    REQUIRED)
get_filename_component(ARM_BIN "${ARM_GCC}" DIRECTORY)

set(CMAKE_C_COMPILER   "${ARM_BIN}/arm-none-eabi-gcc${TOOL_SUFFIX}")
set(CMAKE_ASM_COMPILER "${ARM_BIN}/arm-none-eabi-gcc${TOOL_SUFFIX}")
set(CMAKE_OBJCOPY      "${ARM_BIN}/arm-none-eabi-objcopy${TOOL_SUFFIX}" CACHE FILEPATH "")
set(CMAKE_SIZE         "${ARM_BIN}/arm-none-eabi-size${TOOL_SUFFIX}"    CACHE FILEPATH "")

# Cortex-M4 with the single-precision FPU. The stock firmware enables CP10/CP11 and is
# built hard-float, so we match: a soft-float build would work but would silently discard
# the FPU the startup code goes out of its way to turn on.
set(ARCH_FLAGS "-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb")

set(CMAKE_C_FLAGS_INIT "${ARCH_FLAGS} -ffunction-sections -fdata-sections -fno-common")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${ARCH_FLAGS} -Wl,--gc-sections -nostartfiles")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

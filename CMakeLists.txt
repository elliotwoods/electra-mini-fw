cmake_minimum_required(VERSION 3.20)

# Configure with:
#   cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#   cmake --build build
#
# Produces TWO images:
#
#   bootloader.srec  0x00100000, 128 KB   flashed once via the SD-card route
#   app.srec         0x00120000, 2944 KB  flashed over USB by the bootloader, freely
#
# Neither is finished until it passes tools/mkimage/check-image.js. The stock bootloader
# programs whatever it finds at /BOOT/update.srec without validating it, so that gate is the
# only thing between a bad link and a device that needs a physical recovery cycle.

project(electra-mini-fw C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(COMMON_SOURCES
    src/startup/startup.c
    src/startup/vectors.c
    src/bsp/clock.c
    src/bsp/sdram.c
    src/bsp/ioport.c
    src/bsp/icu.c
    src/bsp/electra_pin_cfg.c
)

set(COMMON_INCLUDES src/bsp src/hal src/common src/boot src/app src/proto src)

set(COMMON_C_FLAGS
    -Og -g3
    -Wall -Wextra -Wshadow -Wundef
    -Wdouble-promotion              # an accidental double on an SP-only FPU is a silent cost
    -Werror=implicit-function-declaration
)

# Shared linker-script directory, so INCLUDE memory.ld resolves from either script.
set(LD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/startup)

# ---------------------------------------------------------------- image helper
function(electra_image target linker_script out_name)
    target_include_directories(${target} PRIVATE ${COMMON_INCLUDES})
    target_compile_options(${target} PRIVATE ${COMMON_C_FLAGS})
    target_link_options(${target} PRIVATE
        -T${LD_DIR}/${linker_script}
        -L${LD_DIR}
        -Wl,-Map=${CMAKE_BINARY_DIR}/${out_name}.map
        -Wl,--print-memory-usage
        --specs=nano.specs
        --specs=nosys.specs
    )
    set_target_properties(${target} PROPERTIES
        LINK_DEPENDS "${LD_DIR}/${linker_script};${LD_DIR}/memory.ld;${LD_DIR}/sections.ld"
        OUTPUT_NAME  "${out_name}.elf"
    )
    # objcopy picks the record width from the highest address; everything is under
    # 0x01000000 so it emits S2/S8 unprompted, matching the stock image's shape.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O srec $<TARGET_FILE:${target}> ${CMAKE_BINARY_DIR}/${out_name}.srec
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${target}>
        COMMAND node ${CMAKE_CURRENT_SOURCE_DIR}/tools/mkimage/check-image.js
                     ${CMAKE_BINARY_DIR}/${out_name}.srec --expect-base ${ARGV3}
        COMMENT "Generating and gating ${out_name}.srec"
        VERBATIM
    )
endfunction()

# ---------------------------------------------------------------- bootloader
add_executable(bootloader
    ${COMMON_SOURCES}
    src/boot/main.c
    src/boot/app_launch.c
    src/boot/usb_service.c
    src/boot/flash_faci.c
    src/hal/usb_fs.c
    src/common/console.c
)
electra_image(bootloader bootloader.ld bootloader 0x00100000)

# ---------------------------------------------------------------- application
add_executable(app
    ${COMMON_SOURCES}
    src/hal/spi.c
    src/hal/ra8876.c
    src/hal/riic.c
    src/hal/qt2120.c
    src/hal/inputs.c
    src/hal/usb_fs.c
    src/common/console.c
    src/boot/app_launch.c
    src/app/ui.c
    src/app/ui_state.c
    src/app/history.c
    src/app/font_data.c
    src/app/text.c
    src/proto/frame.c
    src/proto/session.c
    src/proto/surface.c
    tests/proto_tests.c
    src/main.c
)
# Diagnostic build: leave the RA8876 exactly as the stock bootloader configured it, so the
# working register values can be read off a live chip instead of recovered from disassembly.
#   cmake -B build -DLCD_SKIP_INIT=ON
option(LCD_SKIP_INIT "Do not run our own RA8876 init; inherit the bootloader's" OFF)
target_compile_definitions(app PRIVATE LCD_SKIP_INIT=$<BOOL:${LCD_SKIP_INIT}>)

electra_image(app app.ld app 0x00120000)

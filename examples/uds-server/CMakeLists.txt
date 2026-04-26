# Checkpoint-4a — UDS-on-CAN bring-up on Waveshare RP2350-CAN.
#
# Builds a minimal UDS server that exposes DiagSession + ECUReset +
# ReadDID + TesterPresent over ISO-TP-on-CAN, driven by uds-tiny.
# No SUIT integration yet — that lands in 4b.

cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(sumo_rp2350_uds-server C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(PICO_BOARD pico2 CACHE STRING "Pico board target")
set(PICO_PLATFORM rp2350 CACHE STRING "Pico platform")
pico_sdk_init()

# ── uds-tiny ───────────────────────────────────────────────────────
set(UDS_TINY_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/uds-tiny
    CACHE PATH "Path to uds-tiny")
add_subdirectory(${UDS_TINY_DIR} uds-tiny EXCLUDE_FROM_ALL)

# ── App ────────────────────────────────────────────────────────────
add_executable(sumo_rp2350_uds-server
    main.c
    uds_platform_time.c
    hal/can_hw.c
    hal/mcp2515.c)

target_include_directories(sumo_rp2350_uds-server PRIVATE
    # Local first so our app_config.h / pin_config.h /
    # uds_platform_time.h shadow the lib defaults.
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/hal)

# Tell the uds-tiny libs to pull in our app_config.h overrides.
# Wrapped in escaped quotes because the lib does
# `#include UDS_APP_CONFIG` (not a string-literal in code).
target_compile_definitions(sumo_rp2350_uds-server PRIVATE
    UDS_APP_CONFIG="app_config.h"
    STORE_APP_CONFIG="app_config.h"
    ISOTP_APP_CONFIG="app_config.h"
    PICO_STACK_SIZE=0x1000)

target_link_libraries(sumo_rp2350_uds-server
    pico_stdlib
    hardware_spi
    hardware_gpio
    hardware_watchdog
    uds_tiny::isotp
    uds_tiny::uds
    uds_tiny::store
    uds_tiny::can_hw)

target_link_options(sumo_rp2350_uds-server PRIVATE
    -Wl,--gc-sections)

pico_enable_stdio_usb (sumo_rp2350_uds-server 1)
pico_enable_stdio_uart(sumo_rp2350_uds-server 0)

pico_add_extra_outputs(sumo_rp2350_uds-server)

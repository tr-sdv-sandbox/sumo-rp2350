# Minimal sumo-rp2350 smoke-test app. Builds against an installed
# pico-sdk; PICO_SDK_PATH must be exported in the environment.

cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(sumo_rp2350_minimal C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Pull in pico-sdk and configure for the RP2350.
set(PICO_BOARD pico2 CACHE STRING "Pico board target")
set(PICO_PLATFORM rp2350 CACHE STRING "Pico platform")
pico_sdk_init()

# Conventional layout: ~/dev/sumo-rp2350/ and ~/dev/libsumo/ are
# siblings on disk. Override either path on the cmake command line
# (-DLIBSUMO_DIR=...) if you've cloned them elsewhere.
set(SUMO_RP2350_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..
    CACHE PATH "Path to the sumo-rp2350 binding (this repo's root)")
set(LIBSUMO_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/../../../libsumo
    CACHE PATH "Path to libsumo's onboard tree")

# This example only touches sumo-rp2350's storage_ops + active-slot
# accessor. It uses libsumo's public struct typedefs (sumo_storage_ops_t
# / sumo_platform_ops_t) but not any of libsumo's compiled code, so
# pull in just the headers — no cross-build of libcsuit / libzstd /
# t_cose / qcbor / mbedtls needed to flash this example.
#
# A "full" example that exercises sumo_validate_envelope +
# sumo_process_image will need those deps cross-built against
# Cortex-M33; that's the next bring-up step.
add_library(sumo_onboard_headers INTERFACE)
target_include_directories(sumo_onboard_headers INTERFACE
    ${LIBSUMO_DIR}/onboard/include)

add_subdirectory(${SUMO_RP2350_DIR} sumo-rp2350 EXCLUDE_FROM_ALL)

add_executable(sumo_rp2350_minimal main.c)

target_link_libraries(sumo_rp2350_minimal
    pico_stdlib
    hardware_flash
    hardware_sync
    sumo_onboard_headers
    sumo-rp2350)

pico_enable_stdio_usb (sumo_rp2350_minimal 1)
pico_enable_stdio_uart(sumo_rp2350_minimal 0)

pico_add_extra_outputs(sumo_rp2350_minimal)

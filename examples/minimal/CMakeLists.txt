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

# Sibling components — paths assume the conventional sumo-workspace
# layout: examples/minimal/ → ../.. → sumo-rp2350 → ../libsumo.
set(SUMO_RP2350_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(LIBSUMO_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/../../../libsumo)

# libsumo's onboard library has a Linux-only fall-through that pulls
# in libcurl. The Pico build doesn't have/need it, so wire just the
# onboard sources directly here. We intentionally side-step
# libsumo/onboard/CMakeLists.txt — that file is host-targeted.
add_library(sumo_onboard_pico STATIC
    ${LIBSUMO_DIR}/onboard/src/validator.c
    ${LIBSUMO_DIR}/onboard/src/decryptor.c
    ${LIBSUMO_DIR}/onboard/src/decompressor.c
    ${LIBSUMO_DIR}/onboard/src/policy.c
    ${LIBSUMO_DIR}/onboard/src/orchestrator.c)

target_include_directories(sumo_onboard_pico PUBLIC
    ${LIBSUMO_DIR}/onboard/include)

# libsumo's onboard depends on libcsuit, t_cose, qcbor, libzstd, and a
# crypto backend (OpenSSL or mbedTLS). Those need to be cross-built
# for the Cortex-M33; that's still outstanding. For now this CMake
# file establishes the example's structure — actual flashable
# firmware needs the embedded crypto/zstd toolchain wired up. See
# README for the bring-up checklist.
add_subdirectory(${SUMO_RP2350_DIR} sumo-rp2350 EXCLUDE_FROM_ALL)

add_executable(sumo_rp2350_minimal main.c)

target_link_libraries(sumo_rp2350_minimal
    pico_stdlib
    hardware_flash
    hardware_sync
    sumo_onboard_pico
    sumo-rp2350)

pico_enable_stdio_usb (sumo_rp2350_minimal 1)
pico_enable_stdio_uart(sumo_rp2350_minimal 0)

pico_add_extra_outputs(sumo_rp2350_minimal)

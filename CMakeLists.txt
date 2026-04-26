# sumo-rp2350 — Pico-SDK platform binding for libsumo.
#
# Consumed as an INTERFACE library by the firmware app. The app must:
#   - have called pico_sdk_init() before this file is included
#   - have the libsumo onboard target (`sumo_onboard`) available
#     (typically via add_subdirectory(libsumo))
#   - link `sumo-rp2350` plus the relevant pico-sdk targets
#     (pico_stdlib, hardware_flash, hardware_sync, ...)
#
# This file does not pull in pico-sdk itself; that's a top-level
# concern for the firmware app.

cmake_minimum_required(VERSION 3.13)

add_library(sumo-rp2350 INTERFACE)

target_sources(sumo-rp2350 INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform_rp2350.c)

target_include_directories(sumo-rp2350 INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

# We don't list pico-sdk targets here as link deps because INTERFACE
# libraries can't usefully forward optional/transitive embedded deps —
# the consuming app links them explicitly. See README.md for the
# minimal target_link_libraries() recipe.

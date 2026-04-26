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
# concern for the firmware app. See examples/minimal/ for the recipe.

cmake_minimum_required(VERSION 3.13)

set(LFS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/littlefs)

add_library(sumo-rp2350 INTERFACE)

target_sources(sumo-rp2350 INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform_rp2350.c
    ${LFS_DIR}/lfs.c
    ${LFS_DIR}/lfs_util.c)

target_include_directories(sumo-rp2350 INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LFS_DIR})

# littlefs: keep its assertion machinery active (helps shake out flash
# layout mistakes early) and quiet the noisy traces. Override these
# from the consuming app if a release build wants different defaults.
target_compile_definitions(sumo-rp2350 INTERFACE
    LFS_NO_DEBUG=1
    LFS_NO_WARN=1
    LFS_NO_ERROR=0)

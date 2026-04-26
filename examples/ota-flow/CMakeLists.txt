# Checkpoint-4c — production-style A/B OTA on Waveshare RP2350-CAN.
#
# Builds on 4b's SUIT-over-UDS download with:
#   - RP2350 bootrom A/B partition picking (no custom bootloader)
#   - TBYB safety net for the first trial boot
#   - App-level multi-boot trial counter (survives ignition cycles)
#   - UDS RoutineControls for activate / commit / rollback
#   - Version metadata baked from CMake into the IMAGE_DEF
#
# Build a specific version:
#   cmake -B build -DOTA_VERSION_MAJOR=1 -DOTA_VERSION_MINOR=2
#   cmake --build build --target sumo_rp2350_ota-flow

cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

# ── OTA version (cache vars; passed to picotool / IMAGE_DEF) ─────
# These set the image's version in the IMAGE_DEF block via
# pico-sdk's PICO_CRT0_VERSION_*; the bootrom's A/B picker prefers
# the slot whose IMAGE_DEF carries the higher version. Same numbers
# get baked into a C string for the 0xF195 software-version DID.
set(OTA_VERSION_MAJOR 1 CACHE STRING "Image major version")
set(OTA_VERSION_MINOR 0 CACHE STRING "Image minor version")
set(OTA_VERSION_PATCH 0 CACHE STRING "Image patch version (string only)")
set(OTA_VERSION_STR
    "${OTA_VERSION_MAJOR}.${OTA_VERSION_MINOR}.${OTA_VERSION_PATCH}")

# TBYB images require rom_explicit_buy() on the first flash-update
# boot before the bootrom commits to them. Set OFF for factory
# install (picotool-loaded directly into a slot, must boot first
# time without buy ceremony); set ON for OTA images that arrive
# via flash-update-boot. The cyw43 firmware in pico-sdk uses the
# same "two builds" pattern.
option(OTA_TBYB "Mark IMAGE_DEF as Try-Before-You-Buy" OFF)

message(STATUS "ota-flow building image version ${OTA_VERSION_STR}"
               " TBYB=${OTA_TBYB}")

# pico-sdk's `pico_mbedtls` includes its `pico_mbedtls_config.h` which
# does a plain `#include "mbedtls_config.h"`. Same trick as
# examples/validate: expose our minimal mbedtls config via the include
# path, and point MBEDTLS_PSA_CRYPTO_CONFIG_FILE at our PSA-side
# config so the default psa/crypto_config.h (everything turned on)
# doesn't get pulled in.
set(SUMO_MBEDTLS_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR})
add_compile_definitions(
    MBEDTLS_PSA_CRYPTO_CONFIG_FILE="${SUMO_MBEDTLS_CONFIG_DIR}/psa_crypto_config.h")

project(sumo_rp2350_ota-flow VERSION ${OTA_VERSION_STR} LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(PICO_BOARD pico2 CACHE STRING "Pico board target")
set(PICO_PLATFORM rp2350 CACHE STRING "Pico platform")
pico_sdk_init()

# Sibling-repo layout: ~/dev/libsumo/ + ~/dev/sumo-rp2350/.
set(LIBSUMO_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/../../../libsumo
    CACHE PATH "Path to libsumo")
set(SUMO_RP2350_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..
    CACHE PATH "Path to sumo-rp2350")
set(LIBCSUIT_DIR    ${LIBSUMO_DIR}/3rdparty/libcsuit
    CACHE PATH "Path to libcsuit (in libsumo's 3rdparty)")
set(UDS_TINY_DIR    ${SUMO_RP2350_DIR}/3rdparty/uds-tiny
    CACHE PATH "Path to uds-tiny")

# ── uds-tiny ───────────────────────────────────────────────────────
add_subdirectory(${UDS_TINY_DIR} uds-tiny EXCLUDE_FROM_ALL)

# ── QCBOR ───────────────────────────────────────────────────────────
add_library(qcbor STATIC
    ${LIBCSUIT_DIR}/3rdparty/QCBOR/src/ieee754.c
    ${LIBCSUIT_DIR}/3rdparty/QCBOR/src/qcbor_decode.c
    ${LIBCSUIT_DIR}/3rdparty/QCBOR/src/qcbor_encode.c
    ${LIBCSUIT_DIR}/3rdparty/QCBOR/src/qcbor_err_to_str.c
    ${LIBCSUIT_DIR}/3rdparty/QCBOR/src/UsefulBuf.c)
target_include_directories(qcbor PUBLIC
    ${LIBCSUIT_DIR}/3rdparty/QCBOR/inc)
target_compile_options(qcbor PRIVATE -Os -ffunction-sections -fdata-sections)

# ── t_cose (PSA backend) ────────────────────────────────────────────
file(GLOB TCOSE_SRCS ${LIBCSUIT_DIR}/3rdparty/t_cose/src/*.c)
add_library(t_cose STATIC
    ${TCOSE_SRCS}
    ${LIBCSUIT_DIR}/3rdparty/t_cose/crypto_adapters/t_cose_psa_crypto.c)
target_include_directories(t_cose
    PUBLIC  ${LIBCSUIT_DIR}/3rdparty/t_cose/inc
            ${PICO_MBEDTLS_PATH}/include
            ${SUMO_MBEDTLS_CONFIG_DIR}
    PRIVATE ${LIBCSUIT_DIR}/3rdparty/t_cose/src)
target_link_libraries(t_cose PUBLIC qcbor)
target_compile_definitions(t_cose PUBLIC T_COSE_USE_PSA_CRYPTO=1)
target_compile_options(t_cose PRIVATE -Os -ffunction-sections -fdata-sections)

# ── libcsuit (PSA-mode) ─────────────────────────────────────────────
file(GLOB CSUIT_SRCS ${LIBCSUIT_DIR}/src/*.c)
add_library(csuit STATIC ${CSUIT_SRCS})
target_include_directories(csuit PUBLIC
    ${LIBCSUIT_DIR}/inc
    ${PICO_MBEDTLS_PATH}/include
    ${SUMO_MBEDTLS_CONFIG_DIR})
target_compile_definitions(csuit PUBLIC
    LIBCSUIT_PSA_CRYPTO_C=1
    SUIT_MAX_DEPENDENCY_NUM=4
    SUIT_MAX_ARRAY_LENGTH=24)
target_link_libraries(csuit PUBLIC qcbor t_cose)
target_compile_options(csuit PRIVATE -Os -ffunction-sections -fdata-sections)

# ── libzstd (decoder-only, minified) ────────────────────────────────
set(ZSTD_DIR ${SUMO_RP2350_DIR}/3rdparty/zstd/lib)
add_library(zstd_dec STATIC
    ${ZSTD_DIR}/common/debug.c
    ${ZSTD_DIR}/common/entropy_common.c
    ${ZSTD_DIR}/common/error_private.c
    ${ZSTD_DIR}/common/fse_decompress.c
    ${ZSTD_DIR}/common/pool.c
    ${ZSTD_DIR}/common/threading.c
    ${ZSTD_DIR}/common/xxhash.c
    ${ZSTD_DIR}/common/zstd_common.c
    ${ZSTD_DIR}/decompress/huf_decompress.c
    ${ZSTD_DIR}/decompress/zstd_ddict.c
    ${ZSTD_DIR}/decompress/zstd_decompress_block.c
    ${ZSTD_DIR}/decompress/zstd_decompress.c)
target_include_directories(zstd_dec PUBLIC ${ZSTD_DIR})
target_compile_definitions(zstd_dec PRIVATE
    ZSTD_LIB_MINIFY=1
    ZSTD_LEGACY_SUPPORT=0
    ZSTD_LIB_DICTBUILDER=0
    ZSTD_LIB_DEPRECATED=0
    ZSTD_NO_INLINE=1
    ZSTD_DISABLE_ASM=1
    ZSTD_NOBENCH=1
    XXH_NO_INLINE_HINTS=1)
target_compile_options(zstd_dec PRIVATE -Os -ffunction-sections -fdata-sections)

# ── libsumo onboard (verify + decompress subset) ─────────────────────
add_library(sumo_onboard STATIC
    ${LIBSUMO_DIR}/onboard/src/validator.c
    ${LIBSUMO_DIR}/onboard/src/policy.c
    ${LIBSUMO_DIR}/onboard/src/decompressor.c)
target_include_directories(sumo_onboard
    PUBLIC  ${LIBSUMO_DIR}/onboard/include
    PRIVATE ${LIBSUMO_DIR}/onboard/src)
target_link_libraries(sumo_onboard PUBLIC csuit zstd_dec)
target_compile_definitions(sumo_onboard PRIVATE
    SUMO_DECOMPRESSOR_WINDOW_LOG_MAX=10)
target_compile_options(sumo_onboard PRIVATE -Os -ffunction-sections -fdata-sections)

# ── sumo-rp2350 binding ─────────────────────────────────────────────
add_subdirectory(${SUMO_RP2350_DIR} sumo-rp2350 EXCLUDE_FROM_ALL)

# ── App ────────────────────────────────────────────────────────────
add_executable(sumo_rp2350_ota-flow
    main.c
    uds_platform_time.c
    hal/can_hw.c
    hal/mcp2515.c)

target_include_directories(sumo_rp2350_ota-flow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/hal)

target_compile_definitions(sumo_rp2350_ota-flow PRIVATE
    PICO_STACK_SIZE=0x1000

    # IMAGE_DEF: bake the major/minor into the bootrom-recognised
    # version words so the A/B picker can rank slots by version when
    # there's no FUB in play. PICO_CRT0_VERSION_* are the canonical
    # SDK knobs for this; their values land in the IMAGE_DEF block.
    PICO_CRT0_VERSION_MAJOR=${OTA_VERSION_MAJOR}
    PICO_CRT0_VERSION_MINOR=${OTA_VERSION_MINOR}

    # IMAGE_DEF: TBYB flag. ON for OTA images so the bootrom requires
    # rom_explicit_buy() on the first flash-update boot. OFF for the
    # factory install (which is loaded by picotool, not via FUB, and
    # must boot first time without a buy ceremony).
    PICO_CRT0_IMAGE_TYPE_TBYB=$<IF:$<BOOL:${OTA_TBYB}>,1,0>

    # 0xF195 software-version DID — same string the picker sees in
    # the IMAGE_DEF, exposed back over UDS for the orchestrator.
    OTA_FW_VERSION_STR="${OTA_VERSION_STR}")

target_link_libraries(sumo_rp2350_ota-flow
    pico_stdlib
    hardware_flash
    hardware_spi
    hardware_gpio
    hardware_sync
    hardware_watchdog
    pico_mbedtls
    sumo_onboard
    sumo-rp2350
    uds_tiny::isotp
    uds_tiny::uds
    uds_tiny::store
    uds_tiny::can_hw)

# Same psa_util.c gap as examples/validate — pico-sdk's pico_mbedtls
# source list omits it but mbedtls 3.6.2 needs it for
# mbedtls_psa_get_random / mbedtls_ecc_group_from_psa.
target_sources(sumo_rp2350_ota-flow PRIVATE
    ${PICO_MBEDTLS_PATH}/library/psa_util.c)

target_link_options(sumo_rp2350_ota-flow PRIVATE
    -Wl,--gc-sections)

pico_enable_stdio_usb (sumo_rp2350_ota-flow 1)
pico_enable_stdio_uart(sumo_rp2350_ota-flow 0)

# Add a SHA-256 HASH_DEF block to the IMAGE_DEF metadata. The RP2350
# bootrom uses this to validate the image at boot. Without it, the
# FUB-into-TBYB-image path silently falls back to the prior slot
# (the bootrom can't verify the image). Must be called BEFORE
# pico_add_extra_outputs.
pico_hash_binary(sumo_rp2350_ota-flow)

# Sign the IMAGE_DEF with secp256k1. Empirically (and matching the
# pico-examples picow_ota_update reference), the bootrom's FUB+TBYB
# validation requires a SIGNATURE block in addition to the HASH —
# even on UNSECURED RP2350 with secure_boot=0. With hash-only the
# bootrom records boot_type=F and reboot_p0 correctly but does not
# enter TBYB mode (boot_diagnostic stays 0, partition stays at the
# prior slot). With a secp256k1 signature in the IMAGE_DEF, the
# validation path engages and FUB lands in the staged slot.
pico_sign_binary(sumo_rp2350_ota-flow
    ${CMAKE_CURRENT_SOURCE_DIR}/keys/bootkey.pem)

pico_add_extra_outputs(sumo_rp2350_ota-flow)

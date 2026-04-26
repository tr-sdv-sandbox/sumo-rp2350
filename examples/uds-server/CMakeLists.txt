# Checkpoint-4b — SUIT-over-UDS download on Waveshare RP2350-CAN.
#
# Layers the libsumo SUIT receive path (validate + decrypt + decompress)
# on top of the 4a UDS server. Frames an envelope+payload as
#   [4-byte LE env_len] [envelope] [payload]
# and pushes it via UDS RequestDownload + TransferData + TransferExit.

cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

# pico-sdk's `pico_mbedtls` includes its `pico_mbedtls_config.h` which
# does a plain `#include "mbedtls_config.h"`. Same trick as
# examples/validate: expose our minimal mbedtls config via the include
# path, and point MBEDTLS_PSA_CRYPTO_CONFIG_FILE at our PSA-side
# config so the default psa/crypto_config.h (everything turned on)
# doesn't get pulled in.
set(SUMO_MBEDTLS_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR})
add_compile_definitions(
    MBEDTLS_PSA_CRYPTO_CONFIG_FILE="${SUMO_MBEDTLS_CONFIG_DIR}/psa_crypto_config.h")

project(sumo_rp2350_uds-server C CXX ASM)

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
add_executable(sumo_rp2350_uds-server
    main.c
    uds_platform_time.c
    hal/can_hw.c
    hal/mcp2515.c)

target_include_directories(sumo_rp2350_uds-server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/hal)

target_compile_definitions(sumo_rp2350_uds-server PRIVATE
    PICO_STACK_SIZE=0x1000)

target_link_libraries(sumo_rp2350_uds-server
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
target_sources(sumo_rp2350_uds-server PRIVATE
    ${PICO_MBEDTLS_PATH}/library/psa_util.c)

target_link_options(sumo_rp2350_uds-server PRIVATE
    -Wl,--gc-sections)

pico_enable_stdio_usb (sumo_rp2350_uds-server 1)
pico_enable_stdio_uart(sumo_rp2350_uds-server 0)

pico_add_extra_outputs(sumo_rp2350_uds-server)

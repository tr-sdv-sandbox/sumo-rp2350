# Checkpoint-1 example: cross-build libsumo onboard + libcsuit + t_cose
# + QCBOR for Cortex-M33, link against pico-sdk's mbedtls, run
# sumo_validate_envelope() against a baked-in fixture envelope.
#
# This is the smallest possible working slice: signature + manifest
# digest verification only. No decryption, no decompression, no fetch.
# Once it links + flashes + prints "OK" we have a known-good cross-
# build pipeline to extend in checkpoints 2 & 3.

cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(sumo_rp2350_validate C CXX ASM)

# pico-sdk's `pico_mbedtls` includes its `pico_mbedtls_config.h` which
# does a plain `#include "mbedtls_config.h"`. So we expose our own
# mbedtls_config.h via the include path. For the PSA-side config we
# also have to point MBEDTLS_PSA_CRYPTO_CONFIG_FILE at ours, otherwise
# mbedtls falls back to its own default psa/crypto_config.h (which has
# everything — SHA3, RSA, all curves — turned on, blowing up the .a).
set(SUMO_MBEDTLS_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR})
add_compile_definitions(
    MBEDTLS_PSA_CRYPTO_CONFIG_FILE="${SUMO_MBEDTLS_CONFIG_DIR}/psa_crypto_config.h")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(PICO_BOARD pico2 CACHE STRING "Pico board target")
set(PICO_PLATFORM rp2350 CACHE STRING "Pico platform")
pico_sdk_init()

# Conventional layout: ~/dev/libsumo/ + ~/dev/sumo-rp2350/ as siblings.
set(LIBSUMO_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/../../../libsumo
    CACHE PATH "Path to libsumo")
set(SUMO_RP2350_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..
    CACHE PATH "Path to sumo-rp2350")
set(LIBCSUIT_DIR    ${LIBSUMO_DIR}/3rdparty/libcsuit
    CACHE PATH "Path to libcsuit (in libsumo's 3rdparty)")

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
# t_cose needs mbedtls *headers* but must NOT pull in mbedtls source
# compilation (pico_mbedtls is an INTERFACE that adds sources to its
# consumer; doing that here would compile mbedtls *into* libt_cose.a
# and again into the final executable). Add headers directly; the
# final executable is the only target that links pico_mbedtls.
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
# Note: LIBCSUIT_DISABLE_MAC is *not* set — libcsuit's source still
# references t_cose_mac_validate_ctx unconditionally even when the
# define is on (the gate only covers the bottom half of suit_cose.c
# but not the top). Linker --gc-sections strips the unused MAC paths
# at link time, so leaving them in costs nothing in the final binary.
target_link_libraries(csuit PUBLIC qcbor t_cose)
target_compile_options(csuit PRIVATE -Os -ffunction-sections -fdata-sections)

# ── libsumo onboard (verify-only subset) ────────────────────────────
# checkpoint 1 only needs validator.c + policy.c.
# decryptor.c pulls in <openssl/evp.h>, decompressor.c pulls in
# <zstd.h>, orchestrator.c pulls in OpenSSL too — those land when we
# add the encrypted/compressed paths in checkpoints 2 & 3.
add_library(sumo_onboard STATIC
    ${LIBSUMO_DIR}/onboard/src/validator.c
    ${LIBSUMO_DIR}/onboard/src/policy.c)
target_include_directories(sumo_onboard
    PUBLIC  ${LIBSUMO_DIR}/onboard/include
    PRIVATE ${LIBSUMO_DIR}/onboard/src)
target_link_libraries(sumo_onboard PUBLIC csuit)
target_compile_options(sumo_onboard PRIVATE -Os -ffunction-sections -fdata-sections)

# ── sumo-rp2350 binding ─────────────────────────────────────────────
add_subdirectory(${SUMO_RP2350_DIR} sumo-rp2350 EXCLUDE_FROM_ALL)

# ── App ─────────────────────────────────────────────────────────────
add_executable(sumo_rp2350_validate main.c decryptor_psa.c)
target_include_directories(sumo_rp2350_validate PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})

# Pico-sdk default stacks are 2 KB per core, allocated inside
# SCRATCH_Y (4 KB total). 4 KB is the most we can take without
# moving the stack out of SCRATCH_Y. PSA + ECDSA verify can spike
# to ~3 KB transient stack, so 4 KB removes overflow as a suspect
# while we're chasing silent hangs.
target_compile_definitions(sumo_rp2350_validate PRIVATE
    PICO_STACK_SIZE=0x1000)

target_link_libraries(sumo_rp2350_validate
    pico_stdlib
    hardware_flash
    hardware_sync
    pico_mbedtls
    sumo_onboard
    sumo-rp2350)

# pico-sdk's pico_mbedtls source list (CMakeLists.txt) was written for
# an earlier mbedtls 3.x release and omits psa_util.c, which 3.6.2's
# psa_crypto_ecp.c calls (mbedtls_psa_get_random,
# mbedtls_ecc_group_from_psa). Pull it in explicitly here.
target_sources(sumo_rp2350_validate PRIVATE
    ${PICO_MBEDTLS_PATH}/library/psa_util.c)

# Aggressive dead-code / dead-data removal — relevant to keep the
# unused decryptor/orchestrator paths out of the .uf2.
target_link_options(sumo_rp2350_validate PRIVATE
    -Wl,--gc-sections)

pico_enable_stdio_usb (sumo_rp2350_validate 1)
pico_enable_stdio_uart(sumo_rp2350_validate 0)

pico_add_extra_outputs(sumo_rp2350_validate)

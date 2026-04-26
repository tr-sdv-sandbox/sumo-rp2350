/**
 * @file platform_rp2350.h
 * @brief Pico-SDK platform binding for libsumo on RP2350.
 *
 * Two factories:
 *   - sumo_rp2350_storage_ops()   → sumo_storage_ops_t, persists policy
 *                                   (sumo_seq / sumo_sec_ver / …) in a
 *                                   dedicated flash sector with CRC+magic
 *                                   guards.
 *   - sumo_rp2350_platform_ops()  → sumo_platform_ops_t, lays decrypted
 *                                   firmware into one of two A/B staging
 *                                   slots, tracks active slot in the kv
 *                                   sector, and forwards `fetch` to an
 *                                   integrator-supplied transport hook.
 *
 * The integrator's firmware app owns:
 *   - top-level pico-sdk init (pico_sdk_init())
 *   - flash-region layout (passed in via sumo_rp2350_config_t)
 *   - the transport behind fetch_fn (typically UDS-over-CAN reassembly,
 *     hosted in a separate sibling repo)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef SUMO_PLATFORM_RP2350_H
#define SUMO_PLATFORM_RP2350_H

#include <stddef.h>
#include <stdint.h>

#include "sumo/orchestrator.h"  /* sumo_platform_ops_t */
#include "sumo/policy.h"        /* sumo_storage_ops_t  */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Integrator-supplied fetch callback. Pulls `buf_size` bytes for the
 * given URI from whatever transport the app uses (UDS, USB, …),
 * writing the actual byte count to *fetched. Return 0 on success,
 * negative on error.
 */
typedef int (*sumo_rp2350_fetch_fn)(
    const char *uri, size_t uri_len,
    uint8_t *buf, size_t buf_size, size_t *fetched,
    void *user_ctx);

/**
 * RP2350 platform configuration.
 *
 * All offsets are *XIP-relative* (i.e., what hardware/flash.h's
 * `flash_range_*` calls expect — not absolute XIP_BASE addresses).
 * They must be 4 KB aligned and non-overlapping.
 */
typedef struct {
    uint32_t kv_offset;        /**< where the policy kv blob lives. */
    uint32_t slot_a_offset;    /**< OTA staging slot A start. */
    uint32_t slot_a_size;      /**< OTA staging slot A capacity (bytes). */
    uint32_t slot_b_offset;    /**< OTA staging slot B start. */
    uint32_t slot_b_size;      /**< OTA staging slot B capacity (bytes). */

    /** Integrator transport hook + its opaque context. */
    sumo_rp2350_fetch_fn fetch_fn;
    void                *fetch_ctx;
} sumo_rp2350_config_t;

/**
 * Build a `sumo_storage_ops_t` backed by a single 4 KB flash sector at
 * `cfg->kv_offset`. Returns NULL on bad config.
 *
 * Supported keys (libsumo-defined): "sumo_seq" (u64),
 * "sumo_sec_ver" (u64), "sumo_reject_before" (i64). Other keys
 * return -1 from read/write — by design, since this v1 uses a
 * fixed-layout struct rather than a generic kv-store.
 */
sumo_storage_ops_t *sumo_rp2350_storage_ops(const sumo_rp2350_config_t *cfg);

void sumo_rp2350_storage_ops_free(sumo_storage_ops_t *ops);

/**
 * Build a `sumo_platform_ops_t`. write/swap/invoke target the staging
 * slots in flash; persist_sequence routes through the kv sector;
 * fetch forwards to cfg->fetch_fn.
 */
sumo_platform_ops_t *sumo_rp2350_platform_ops(const sumo_rp2350_config_t *cfg);

void sumo_rp2350_platform_ops_free(sumo_platform_ops_t *ops);

/**
 * Read which staging slot is currently "active" (the integrator's app
 * jumps to this slot at boot). Convenient for boot-time decisions.
 *
 * @return 0 for slot A, 1 for slot B, negative on error.
 */
int sumo_rp2350_active_slot(const sumo_rp2350_config_t *cfg);

#ifdef __cplusplus
}
#endif
#endif /* SUMO_PLATFORM_RP2350_H */

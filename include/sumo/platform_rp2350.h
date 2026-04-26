/**
 * @file platform_rp2350.h
 * @brief Pico-SDK platform binding for libsumo on RP2350.
 *
 * Single shared handle (`sumo_rp2350_t`) owns the littlefs mount over
 * the configured kv region, plus knowledge of the A/B firmware staging
 * slots. Both libsumo callback bundles —
 *   - sumo_storage_ops_t (libsumo policy: sumo_seq, sumo_sec_ver, …)
 *   - sumo_platform_ops_t (orchestrator I/O: fetch/write/swap/invoke)
 * are vended off that handle so they share the same filesystem cursor.
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
 *
 * `fs_size` is reserved exclusively for the littlefs mount; it must
 * be a multiple of FLASH_SECTOR_SIZE (4 KB) and at least 8 KB so
 * littlefs has two blocks for its metadata pair.
 */
typedef struct {
    uint32_t fs_offset;        /**< littlefs region start (4 KB aligned). */
    uint32_t fs_size;          /**< littlefs region size (>= 8192). */
    uint32_t slot_a_offset;    /**< OTA staging slot A start. */
    uint32_t slot_a_size;      /**< OTA staging slot A capacity (bytes). */
    uint32_t slot_b_offset;    /**< OTA staging slot B start. */
    uint32_t slot_b_size;      /**< OTA staging slot B capacity (bytes). */

    /** Integrator transport hook + its opaque context. */
    sumo_rp2350_fetch_fn fetch_fn;
    void                *fetch_ctx;
} sumo_rp2350_config_t;

/** Opaque shared handle owning the littlefs mount + slot bookkeeping. */
typedef struct sumo_rp2350 sumo_rp2350_t;

/**
 * Create a binding handle, mount (or format-then-mount on first run)
 * the littlefs filesystem in `cfg->fs_offset..fs_offset+fs_size`.
 *
 * Returns NULL on bad config or mount failure.
 */
sumo_rp2350_t *sumo_rp2350_create(const sumo_rp2350_config_t *cfg);

/** Unmount the filesystem and free the handle. */
void sumo_rp2350_free(sumo_rp2350_t *r);

/**
 * Vend a `sumo_storage_ops_t`. Lifetime is tied to `r`; do *not*
 * pass to `sumo_linux_storage_ops_free`-style helpers. Multiple calls
 * return the same bundle.
 */
sumo_storage_ops_t  *sumo_rp2350_storage_ops (sumo_rp2350_t *r);

/** Vend a `sumo_platform_ops_t`. Same lifetime contract. */
sumo_platform_ops_t *sumo_rp2350_platform_ops(sumo_rp2350_t *r);

/**
 * Read which staging slot is currently "active" (the integrator's app
 * jumps to this slot at boot). Returns 0 (slot A) by default if the
 * fs has no record yet; -1 on error.
 */
int sumo_rp2350_active_slot(sumo_rp2350_t *r);

#ifdef __cplusplus
}
#endif
#endif /* SUMO_PLATFORM_RP2350_H */

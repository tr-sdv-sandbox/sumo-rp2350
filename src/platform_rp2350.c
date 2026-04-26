/**
 * @file platform_rp2350.c
 * @brief Pico-SDK implementation of libsumo's platform_ops + storage_ops
 *        for RP2350-class boards. See platform_rp2350.h for the contract.
 *
 * Storage backend is littlefs (vendored at 3rdparty/littlefs). Each
 * libsumo policy key (`sumo_seq`, `sumo_sec_ver`, `sumo_reject_before`)
 * maps to a small file at `/<key>` containing 8 bytes little-endian.
 * The active slot byte lives at `/active_slot`. Arbitrary additional
 * keys work too — the layer is fully generic — though libsumo today
 * only writes the three above.
 *
 * Component writes go to *raw* flash slots (slot_a / slot_b), not into
 * littlefs, since firmware images are large and we want page-direct
 * writes without the fs metadata overhead.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sumo/platform_rp2350.h"

#include <stdio.h>   /* snprintf */
#include <stdlib.h>
#include <string.h>

#include "lfs.h"

/* Pico SDK headers — only available when built inside a pico-sdk
 * project. Intentionally not gated; if you're compiling this on a
 * Linux dev host the build should fail loudly rather than silently
 * stub out. */
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/platform.h"

#define KV_BLOCK_SIZE     FLASH_SECTOR_SIZE   /* 4096 */
#define KV_PAGE_SIZE      FLASH_PAGE_SIZE     /* 256  */
#define KV_LOOKAHEAD_SIZE 64

/* --- Shared handle (owns the lfs mount; vended ops bundles share it). */

struct sumo_rp2350 {
    sumo_rp2350_config_t cfg;

    /* littlefs state. */
    lfs_t              lfs;
    struct lfs_config  lfscfg;
    uint8_t            read_buf [KV_PAGE_SIZE];
    uint8_t            prog_buf [KV_PAGE_SIZE];
    uint8_t            lookahead_buf[KV_LOOKAHEAD_SIZE];
    int                mounted;

    /* Pre-built ops bundles, vended by accessors. */
    sumo_storage_ops_t  storage_ops;
    sumo_platform_ops_t platform_ops;
};

/* --- littlefs block-device callbacks (forward to flash_range_*). */

static int bd_read(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, void *buffer, lfs_size_t size)
{
    sumo_rp2350_t *r = c->context;
    uint32_t addr = r->cfg.fs_offset + block * c->block_size + off;
    /* Flash is XIP-mapped at XIP_BASE — direct memcpy from there is
     * the fastest path and avoids touching the QSPI command queue. */
    memcpy(buffer, (const uint8_t *)(XIP_BASE + addr), size);
    return 0;
}

static int bd_prog(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, const void *buffer, lfs_size_t size)
{
    sumo_rp2350_t *r = c->context;
    uint32_t addr = r->cfg.fs_offset + block * c->block_size + off;
    uint32_t saved = save_and_disable_interrupts();
    flash_range_program(addr, buffer, size);
    restore_interrupts(saved);
    return 0;
}

static int bd_erase(const struct lfs_config *c, lfs_block_t block)
{
    sumo_rp2350_t *r = c->context;
    uint32_t addr = r->cfg.fs_offset + block * c->block_size;
    uint32_t saved = save_and_disable_interrupts();
    flash_range_erase(addr, c->block_size);
    restore_interrupts(saved);
    return 0;
}

static int bd_sync(const struct lfs_config *c) { (void)c; return 0; }

/* --- Mount: try, format-on-failure, retry. --- */

static int rp2350_mount(sumo_rp2350_t *r)
{
    if (r->mounted) return 0;
    r->lfscfg = (struct lfs_config){
        .context        = r,
        .read           = bd_read,
        .prog           = bd_prog,
        .erase          = bd_erase,
        .sync           = bd_sync,
        .read_size      = KV_PAGE_SIZE,
        .prog_size      = KV_PAGE_SIZE,
        .block_size     = KV_BLOCK_SIZE,
        .block_count    = r->cfg.fs_size / KV_BLOCK_SIZE,
        .cache_size     = KV_PAGE_SIZE,
        .lookahead_size = KV_LOOKAHEAD_SIZE,
        .read_buffer    = r->read_buf,
        .prog_buffer    = r->prog_buf,
        .lookahead_buffer = r->lookahead_buf,
        .block_cycles   = 100,
    };
    int rc = lfs_mount(&r->lfs, &r->lfscfg);
    if (rc < 0) {
        /* Unformatted region — format and remount once. */
        rc = lfs_format(&r->lfs, &r->lfscfg);
        if (rc < 0) return -1;
        rc = lfs_mount(&r->lfs, &r->lfscfg);
        if (rc < 0) return -1;
    }
    r->mounted = 1;
    return 0;
}

/* --- Generic kv read/write (used by both storage_ops and active-slot). */

static int kv_read_bytes(sumo_rp2350_t *r,
                         const char *key, void *out, size_t want)
{
    char path[64];
    if (snprintf(path, sizeof(path), "/%s", key) >= (int)sizeof(path))
        return -1;

    lfs_file_t f;
    if (lfs_file_open(&r->lfs, &f, path, LFS_O_RDONLY) < 0) return -1;
    lfs_ssize_t got = lfs_file_read(&r->lfs, &f, out, want);
    lfs_file_close(&r->lfs, &f);
    return ((size_t)got == want) ? 0 : -1;
}

static int kv_write_bytes(sumo_rp2350_t *r,
                          const char *key, const void *data, size_t len)
{
    char path[64];
    if (snprintf(path, sizeof(path), "/%s", key) >= (int)sizeof(path))
        return -1;

    lfs_file_t f;
    int rc = lfs_file_open(&r->lfs, &f, path,
                           LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (rc < 0) return -1;
    lfs_ssize_t wrote = lfs_file_write(&r->lfs, &f, data, len);
    int closerc = lfs_file_close(&r->lfs, &f);
    if (wrote != (lfs_ssize_t)len || closerc < 0) return -1;
    return 0;
}

/* --- storage_ops callbacks (8-byte LE for u64 / i64). --- */

static int store_read_u64(const char *key, uint64_t *value, void *user_ctx)
{
    sumo_rp2350_t *r = user_ctx;
    uint8_t buf[8];
    if (kv_read_bytes(r, key, buf, sizeof(buf)) != 0) return -1;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)buf[i]) << (8 * i);
    *value = v;
    return 0;
}

static int store_write_u64(const char *key, uint64_t value, void *user_ctx)
{
    sumo_rp2350_t *r = user_ctx;
    uint8_t buf[8];
    for (int i = 0; i < 8; i++) buf[i] = (uint8_t)(value >> (8 * i));
    return kv_write_bytes(r, key, buf, sizeof(buf));
}

static int store_read_i64(const char *key, int64_t *value, void *user_ctx)
{
    uint64_t u;
    int rc = store_read_u64(key, &u, user_ctx);
    if (rc != 0) return rc;
    *value = (int64_t)u;
    return 0;
}

static int store_write_i64(const char *key, int64_t value, void *user_ctx)
{
    return store_write_u64(key, (uint64_t)value, user_ctx);
}

/* --- Active slot (single-byte file at /active_slot). --- */

static uint8_t active_slot(sumo_rp2350_t *r)
{
    uint8_t s = 0;
    if (kv_read_bytes(r, "active_slot", &s, 1) != 0) return 0;
    return s ? 1 : 0;
}

static int set_active_slot(sumo_rp2350_t *r, uint8_t s)
{
    return kv_write_bytes(r, "active_slot", &s, 1);
}

/* --- platform_ops callbacks. --- */

static uint32_t inactive_offset(const sumo_rp2350_config_t *cfg, uint8_t active)
{
    return active == 0 ? cfg->slot_b_offset : cfg->slot_a_offset;
}

static uint32_t inactive_size(const sumo_rp2350_config_t *cfg, uint8_t active)
{
    return active == 0 ? cfg->slot_b_size : cfg->slot_a_size;
}

/* OTA writes always target the *inactive* slot — the running image is
 * untouched. The orchestrator hands us 4 KB-aligned chunks; the first
 * chunk (offset == 0) triggers a slot-wide erase. The final chunk may
 * be short — pad to FLASH_PAGE_SIZE with 0xFF before programming. */
static int plat_write(const uint8_t *cid, size_t cid_len,
                      size_t offset,
                      const uint8_t *data, size_t data_len,
                      void *user_ctx)
{
    (void)cid; (void)cid_len;
    sumo_rp2350_t *r = user_ctx;
    uint8_t s = active_slot(r);
    uint32_t base = inactive_offset(&r->cfg, s);
    uint32_t cap  = inactive_size  (&r->cfg, s);

    if (offset > cap || data_len > cap - offset) return -1;
    if ((offset % FLASH_PAGE_SIZE) != 0) return -1;

    uint32_t saved = save_and_disable_interrupts();
    if (offset == 0) {
        flash_range_erase(base, (cap + FLASH_SECTOR_SIZE - 1)
                                 & ~(FLASH_SECTOR_SIZE - 1));
    }
    if (data_len % FLASH_PAGE_SIZE) {
        uint8_t page[FLASH_PAGE_SIZE];
        size_t whole = data_len & ~(FLASH_PAGE_SIZE - 1);
        size_t tail  = data_len - whole;
        if (whole) flash_range_program(base + offset, data, whole);
        memset(page, 0xFF, sizeof(page));
        memcpy(page, data + whole, tail);
        flash_range_program(base + offset + whole, page, FLASH_PAGE_SIZE);
    } else {
        flash_range_program(base + offset, data, data_len);
    }
    restore_interrupts(saved);
    return 0;
}

static int plat_swap(const uint8_t *a, size_t a_len,
                     const uint8_t *b, size_t b_len,
                     void *user_ctx)
{
    /* SUIT swap is per-component; v1 tracks a single global active
     * slot since RP2350 ECUs typically run a single firmware image.
     * Multi-component devices can override / extend later. */
    (void)a; (void)a_len; (void)b; (void)b_len;
    sumo_rp2350_t *r = user_ctx;
    return set_active_slot(r, active_slot(r) == 0 ? 1 : 0);
}

static int plat_invoke(const uint8_t *cid, size_t cid_len, void *user_ctx)
{
    /* No reset here. The integrator decides when to reboot — typically
     * after a UDS RoutineControl(activate) or an explicit ECUReset
     * request. */
    (void)cid; (void)cid_len; (void)user_ctx;
    return 0;
}

static int plat_persist_sequence(const uint8_t *cid, size_t cid_len,
                                 uint64_t seq, void *user_ctx)
{
    (void)cid; (void)cid_len;
    return store_write_u64("sumo_seq", seq, user_ctx);
}

static int plat_fetch(const char *uri, size_t uri_len,
                      uint8_t *buf, size_t buf_size, size_t *fetched,
                      void *user_ctx)
{
    sumo_rp2350_t *r = user_ctx;
    if (!r->cfg.fetch_fn) return -1;
    return r->cfg.fetch_fn(uri, uri_len, buf, buf_size, fetched,
                           r->cfg.fetch_ctx);
}

/* --- Public API. --- */

sumo_rp2350_t *sumo_rp2350_create(const sumo_rp2350_config_t *cfg)
{
    if (!cfg) return NULL;
    if (cfg->fs_offset    % KV_BLOCK_SIZE) return NULL;
    if (cfg->fs_size      % KV_BLOCK_SIZE) return NULL;
    if (cfg->fs_size      < 2 * KV_BLOCK_SIZE) return NULL;
    if (cfg->slot_a_offset % KV_BLOCK_SIZE) return NULL;
    if (cfg->slot_b_offset % KV_BLOCK_SIZE) return NULL;

    sumo_rp2350_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->cfg = *cfg;

    if (rp2350_mount(r) != 0) { free(r); return NULL; }

    r->storage_ops.read_u64  = store_read_u64;
    r->storage_ops.write_u64 = store_write_u64;
    r->storage_ops.read_i64  = store_read_i64;
    r->storage_ops.write_i64 = store_write_i64;
    r->storage_ops.ctx       = r;

    r->platform_ops.fetch            = plat_fetch;
    r->platform_ops.write            = plat_write;
    r->platform_ops.invoke           = plat_invoke;
    r->platform_ops.swap             = plat_swap;
    r->platform_ops.persist_sequence = plat_persist_sequence;
    r->platform_ops.user_ctx         = r;

    return r;
}

void sumo_rp2350_free(sumo_rp2350_t *r)
{
    if (!r) return;
    if (r->mounted) lfs_unmount(&r->lfs);
    free(r);
}

sumo_storage_ops_t *sumo_rp2350_storage_ops(sumo_rp2350_t *r)
{
    return r ? &r->storage_ops : NULL;
}

sumo_platform_ops_t *sumo_rp2350_platform_ops(sumo_rp2350_t *r)
{
    return r ? &r->platform_ops : NULL;
}

int sumo_rp2350_active_slot(sumo_rp2350_t *r)
{
    if (!r) return -1;
    return active_slot(r);
}

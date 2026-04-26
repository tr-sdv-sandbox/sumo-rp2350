/**
 * @file platform_rp2350.c
 * @brief Pico-SDK implementation of libsumo's platform_ops + storage_ops
 *        for RP2350-class boards. See platform_rp2350.h for the contract.
 *
 * Storage layout (single 4 KB flash sector at cfg->kv_offset):
 *
 *   struct sumo_kv_v1 {
 *       uint32_t magic;             // 'SUMO'
 *       uint32_t version;           // 1
 *       uint64_t sumo_seq;          // anti-rollback sequence number
 *       uint64_t sumo_sec_ver;      // security_version floor
 *        int64_t sumo_reject_before;// timestamp revocation
 *       uint8_t  active_slot;       // 0 == A, 1 == B
 *       uint8_t  reserved[3];
 *       uint32_t crc32;             // over preceding bytes
 *   };
 *
 * Each write rewrites the entire sector (erase + program). For the
 * three known SUIT policy keys plus the active-slot byte, the wear
 * cost is bounded — RP2350 flash is rated 100k+ erase cycles per
 * sector and OTA writes at most a handful per update.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sumo/platform_rp2350.h"

#include <stdlib.h>
#include <string.h>

/* Pico SDK headers — only available when consumed from a pico-sdk
 * build. Not present on a Linux dev host; intentionally not gated, so
 * any attempt to compile this file outside that context fails loudly
 * rather than silently producing a stub. */
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/platform.h"

#define SUMO_KV_MAGIC   0x4F4D5553u  /* 'SUMO' little-endian */
#define SUMO_KV_VERSION 1u

#define KV_SECTOR_SIZE FLASH_SECTOR_SIZE   /* 4096 from pico-sdk */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint64_t sumo_seq;
    uint64_t sumo_sec_ver;
     int64_t sumo_reject_before;
    uint8_t  active_slot;
    uint8_t  reserved[3];
    uint32_t crc32;
} sumo_kv_v1_t;

_Static_assert(sizeof(sumo_kv_v1_t) <= KV_SECTOR_SIZE,
               "kv blob must fit in one flash sector");

/* --- Shared context --- */

typedef struct {
    sumo_rp2350_config_t cfg;
} rp2350_ctx_t;

static rp2350_ctx_t *ctx_create(const sumo_rp2350_config_t *cfg)
{
    if (!cfg) return NULL;
    if (cfg->kv_offset % KV_SECTOR_SIZE) return NULL;
    if (cfg->slot_a_offset % KV_SECTOR_SIZE) return NULL;
    if (cfg->slot_b_offset % KV_SECTOR_SIZE) return NULL;
    rp2350_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->cfg = *cfg;
    return c;
}

static void ctx_destroy(rp2350_ctx_t *c) { free(c); }

/* --- CRC-32 (IEEE 802.3 / zlib polynomial, no table — small + cold) --- */

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t len)
{
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(int32_t)(crc & 1u));
    }
    return ~crc;
}

/* --- KV sector access --- */

static const sumo_kv_v1_t *kv_at(uint32_t offset)
{
    /* Direct XIP read of the flash sector. */
    return (const sumo_kv_v1_t *)(XIP_BASE + offset);
}

static int kv_load(const sumo_rp2350_config_t *cfg, sumo_kv_v1_t *out)
{
    const sumo_kv_v1_t *p = kv_at(cfg->kv_offset);
    if (p->magic != SUMO_KV_MAGIC || p->version != SUMO_KV_VERSION)
        return -1;

    /* CRC over everything except the trailing crc32 word itself. */
    size_t crc_len = sizeof(sumo_kv_v1_t) - sizeof(uint32_t);
    uint32_t expected = crc32_update(0, (const uint8_t *)p, crc_len);
    if (p->crc32 != expected) return -1;

    *out = *p;
    return 0;
}

static int kv_store(const sumo_rp2350_config_t *cfg, const sumo_kv_v1_t *blob)
{
    /* Build the sector image with a fresh CRC. */
    uint8_t buf[KV_SECTOR_SIZE];
    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, blob, sizeof(*blob));
    sumo_kv_v1_t *out = (sumo_kv_v1_t *)buf;
    out->magic = SUMO_KV_MAGIC;
    out->version = SUMO_KV_VERSION;
    size_t crc_len = sizeof(sumo_kv_v1_t) - sizeof(uint32_t);
    out->crc32 = crc32_update(0, buf, crc_len);

    /* Critical section while we erase + program: any flash access
     * (including XIP instruction fetch) must be paused. */
    uint32_t saved = save_and_disable_interrupts();
    flash_range_erase(cfg->kv_offset, KV_SECTOR_SIZE);
    flash_range_program(cfg->kv_offset, buf, KV_SECTOR_SIZE);
    restore_interrupts(saved);
    return 0;
}

/* Load existing blob (or fall back to a zeroed/default one if magic /
 * crc are bad), apply the supplied mutation, persist. */
typedef void (*kv_mutate_fn)(sumo_kv_v1_t *kv, void *arg);
static int kv_update(const sumo_rp2350_config_t *cfg,
                     kv_mutate_fn mutate, void *arg)
{
    sumo_kv_v1_t blob;
    if (kv_load(cfg, &blob) != 0) {
        memset(&blob, 0, sizeof(blob));
    }
    mutate(&blob, arg);
    return kv_store(cfg, &blob);
}

/* --- storage_ops callbacks --- */

/* Map known libsumo policy keys to fields in sumo_kv_v1_t. */
static int kv_field_u64(const char *key, uint64_t *value, const sumo_kv_v1_t *kv)
{
    if (!strcmp(key, "sumo_seq"))     { *value = kv->sumo_seq;     return 0; }
    if (!strcmp(key, "sumo_sec_ver")) { *value = kv->sumo_sec_ver; return 0; }
    return -1;
}

static int kv_field_i64(const char *key, int64_t *value, const sumo_kv_v1_t *kv)
{
    if (!strcmp(key, "sumo_reject_before")) {
        *value = kv->sumo_reject_before;
        return 0;
    }
    return -1;
}

static int store_read_u64(const char *key, uint64_t *value, void *user_ctx)
{
    rp2350_ctx_t *c = user_ctx;
    sumo_kv_v1_t blob;
    if (kv_load(&c->cfg, &blob) != 0) return -1;
    return kv_field_u64(key, value, &blob);
}

static int store_read_i64(const char *key, int64_t *value, void *user_ctx)
{
    rp2350_ctx_t *c = user_ctx;
    sumo_kv_v1_t blob;
    if (kv_load(&c->cfg, &blob) != 0) return -1;
    return kv_field_i64(key, value, &blob);
}

typedef struct { const char *key; uint64_t value; int ok; } write_u64_arg_t;

static void mutate_write_u64(sumo_kv_v1_t *kv, void *vp)
{
    write_u64_arg_t *a = vp;
    a->ok = 0;
    if (!strcmp(a->key, "sumo_seq"))     { kv->sumo_seq     = a->value; a->ok = 1; }
    else if (!strcmp(a->key, "sumo_sec_ver")) { kv->sumo_sec_ver = a->value; a->ok = 1; }
}

static int store_write_u64(const char *key, uint64_t value, void *user_ctx)
{
    rp2350_ctx_t *c = user_ctx;
    write_u64_arg_t a = { .key = key, .value = value, .ok = 0 };
    if (kv_update(&c->cfg, mutate_write_u64, &a) != 0) return -1;
    return a.ok ? 0 : -1;
}

typedef struct { const char *key; int64_t value; int ok; } write_i64_arg_t;

static void mutate_write_i64(sumo_kv_v1_t *kv, void *vp)
{
    write_i64_arg_t *a = vp;
    a->ok = 0;
    if (!strcmp(a->key, "sumo_reject_before")) {
        kv->sumo_reject_before = a->value;
        a->ok = 1;
    }
}

static int store_write_i64(const char *key, int64_t value, void *user_ctx)
{
    rp2350_ctx_t *c = user_ctx;
    write_i64_arg_t a = { .key = key, .value = value, .ok = 0 };
    if (kv_update(&c->cfg, mutate_write_i64, &a) != 0) return -1;
    return a.ok ? 0 : -1;
}

/* --- platform_ops callbacks --- */

static uint32_t inactive_slot_offset(const sumo_rp2350_config_t *cfg, uint8_t active)
{
    return active == 0 ? cfg->slot_b_offset : cfg->slot_a_offset;
}

static uint32_t inactive_slot_size(const sumo_rp2350_config_t *cfg, uint8_t active)
{
    return active == 0 ? cfg->slot_b_size : cfg->slot_a_size;
}

/* For OTA, fresh writes always target the *inactive* slot — the
 * currently-running image stays untouched. write() accumulates pages
 * via flash_range_program(); the host orchestrator hands us page-
 * aligned chunks (the libsumo orchestrator reassembles 4 KB chunks). */
static int plat_write(const uint8_t *cid, size_t cid_len,
                      size_t offset,
                      const uint8_t *data, size_t data_len,
                      void *user_ctx)
{
    (void)cid; (void)cid_len;
    rp2350_ctx_t *c = user_ctx;

    sumo_kv_v1_t blob;
    if (kv_load(&c->cfg, &blob) != 0) memset(&blob, 0, sizeof(blob));
    uint32_t base = inactive_slot_offset(&c->cfg, blob.active_slot);
    uint32_t cap  = inactive_slot_size(&c->cfg, blob.active_slot);

    if (offset > cap || data_len > cap - offset) return -1;
    if ((offset % FLASH_PAGE_SIZE) != 0) return -1;

    /* Erase any sectors this chunk newly enters. The simple approach:
     * erase the sector at the start of every aligned-page region the
     * caller hasn't touched yet. Caller signals "first chunk" by
     * passing offset == 0; we erase the whole inactive slot then. */
    uint32_t saved = save_and_disable_interrupts();
    if (offset == 0) {
        flash_range_erase(base, (cap + FLASH_SECTOR_SIZE - 1)
                                 & ~(FLASH_SECTOR_SIZE - 1));
    }

    /* flash_range_program needs a multiple of FLASH_PAGE_SIZE; the
     * orchestrator's 4 KB chunks already match. The final chunk may
     * be short — pad with 0xFF so the page programs cleanly. */
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

static void mutate_set_active(sumo_kv_v1_t *kv, void *vp)
{
    kv->active_slot = *(uint8_t *)vp;
}

static int plat_swap(const uint8_t *a, size_t a_len,
                     const uint8_t *b, size_t b_len,
                     void *user_ctx)
{
    /* SUIT's swap directive flips A/B for the same component. We track
     * a single global active-slot flag rather than per-component
     * because v1 expects a single firmware image. */
    (void)a; (void)a_len; (void)b; (void)b_len;
    rp2350_ctx_t *c = user_ctx;

    sumo_kv_v1_t blob;
    if (kv_load(&c->cfg, &blob) != 0) memset(&blob, 0, sizeof(blob));
    uint8_t flipped = blob.active_slot == 0 ? 1 : 0;
    return kv_update(&c->cfg, mutate_set_active, &flipped);
}

static int plat_invoke(const uint8_t *cid, size_t cid_len, void *user_ctx)
{
    /* No actual reset here — let the integrator decide when to reboot
     * (often after a "RoutineControl: activate" UDS request). */
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
    rp2350_ctx_t *c = user_ctx;
    if (!c->cfg.fetch_fn) return -1;
    return c->cfg.fetch_fn(uri, uri_len, buf, buf_size, fetched,
                           c->cfg.fetch_ctx);
}

/* --- Public factories --- */

sumo_storage_ops_t *sumo_rp2350_storage_ops(const sumo_rp2350_config_t *cfg)
{
    rp2350_ctx_t *c = ctx_create(cfg);
    if (!c) return NULL;
    sumo_storage_ops_t *ops = calloc(1, sizeof(*ops));
    if (!ops) { ctx_destroy(c); return NULL; }
    ops->read_u64  = store_read_u64;
    ops->write_u64 = store_write_u64;
    ops->read_i64  = store_read_i64;
    ops->write_i64 = store_write_i64;
    ops->ctx       = c;
    return ops;
}

void sumo_rp2350_storage_ops_free(sumo_storage_ops_t *ops)
{
    if (!ops) return;
    ctx_destroy(ops->ctx);
    free(ops);
}

sumo_platform_ops_t *sumo_rp2350_platform_ops(const sumo_rp2350_config_t *cfg)
{
    rp2350_ctx_t *c = ctx_create(cfg);
    if (!c) return NULL;
    sumo_platform_ops_t *ops = calloc(1, sizeof(*ops));
    if (!ops) { ctx_destroy(c); return NULL; }
    ops->fetch            = plat_fetch;
    ops->write            = plat_write;
    ops->invoke           = plat_invoke;
    ops->swap             = plat_swap;
    ops->persist_sequence = plat_persist_sequence;
    ops->user_ctx         = c;
    return ops;
}

void sumo_rp2350_platform_ops_free(sumo_platform_ops_t *ops)
{
    if (!ops) return;
    ctx_destroy(ops->user_ctx);
    free(ops);
}

int sumo_rp2350_active_slot(const sumo_rp2350_config_t *cfg)
{
    if (!cfg) return -1;
    sumo_kv_v1_t blob;
    if (kv_load(cfg, &blob) != 0) return 0;  /* default to slot A */
    return blob.active_slot;
}

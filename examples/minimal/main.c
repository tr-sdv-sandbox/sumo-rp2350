/**
 * @file examples/minimal/main.c
 * @brief Smoke-test app for sumo-rp2350.
 *
 * Boots, mounts the sumo kv filesystem on flash, increments
 * `sumo_seq` once per boot, prints what it found, and parks.
 * Demonstrates the full sumo-rp2350 binding without needing a real
 * UDS transport — `fetch_fn` is wired to a stub that always errors
 * (we don't actually run sumo_process_image here).
 *
 * Build (PICO_SDK_PATH must be exported):
 *   mkdir build && cd build && cmake .. && make
 *   picotool load sumo_rp2350_minimal.uf2
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "sumo/platform_rp2350.h"

/* Stub transport: real apps wire this to UDS / CAN-ISO-TP / USB-CDC /
 * etc. For this example we just refuse all fetches so the binding
 * itself is exercised without needing a server. */
static int fetch_unimplemented(const char *uri, size_t uri_len,
                               uint8_t *buf, size_t buf_size, size_t *fetched,
                               void *ctx)
{
    (void)uri; (void)uri_len;
    (void)buf; (void)buf_size; (void)fetched;
    (void)ctx;
    return -1;
}

/* Default flash layout for a 4 MB device. Matches the table in the
 * top-level README — adjust to your board's free regions. */
static const sumo_rp2350_config_t kDefaultCfg = {
    .fs_offset     = 0x003F'0000,  /* 16 KB filesystem region */
    .fs_size       = 0x0000'4000,
    .slot_a_offset = 0x0010'0000,
    .slot_a_size   = 0x0010'0000,
    .slot_b_offset = 0x0020'0000,
    .slot_b_size   = 0x0010'0000,
    .fetch_fn      = fetch_unimplemented,
    .fetch_ctx     = NULL,
};

int main(void)
{
    stdio_init_all();
    /* Give the USB-CDC host time to enumerate before we start
     * printing — otherwise the first lines disappear. */
    sleep_ms(2000);

    printf("\n--- sumo-rp2350 minimal ---\n");

    sumo_rp2350_t *r = sumo_rp2350_create(&kDefaultCfg);
    if (!r) {
        printf("sumo_rp2350_create FAILED — check flash layout.\n");
        while (1) sleep_ms(1000);
    }
    printf("kv mounted, active_slot = %d\n", sumo_rp2350_active_slot(r));

    sumo_storage_ops_t *st = sumo_rp2350_storage_ops(r);

    /* Read prior boot count, increment, write back. Demonstrates the
     * read-modify-write cycle policy_save uses. */
    uint64_t boot = 0;
    if (st->read_u64("boot_count", &boot, st->ctx) == 0) {
        printf("prior boot_count = %llu\n", (unsigned long long)boot);
    } else {
        printf("first boot — boot_count not yet stored\n");
    }
    boot++;
    if (st->write_u64("boot_count", boot, st->ctx) != 0) {
        printf("write FAILED\n");
    } else {
        printf("boot_count now = %llu\n", (unsigned long long)boot);
    }

    /* Show the libsumo policy slots too — they should all be zero on
     * a fresh device. After a real OTA, sumo_policy_save will have
     * populated sumo_seq and sumo_sec_ver via the same backend. */
    uint64_t seq = 0, sec = 0;
    int64_t  rev = 0;
    int rc1 = st->read_u64("sumo_seq",     &seq, st->ctx);
    int rc2 = st->read_u64("sumo_sec_ver", &sec, st->ctx);
    int rc3 = st->read_i64("sumo_reject_before", &rev, st->ctx);
    printf("sumo_seq=%s%llu sumo_sec_ver=%s%llu reject_before=%s%lld\n",
           rc1 ? "(none) " : "", (unsigned long long)seq,
           rc2 ? "(none) " : "", (unsigned long long)sec,
           rc3 ? "(none) " : "", (long long)rev);

    printf("idle.\n");
    while (1) {
        sleep_ms(1000);
    }
    /* unreachable */
    sumo_rp2350_free(r);
    return 0;
}

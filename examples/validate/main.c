/**
 * @file examples/validate/main.c
 * @brief Checkpoint-3: validate + stream-decrypt + stream-decompress.
 *
 *   1. validate the SUIT envelope (ES256, mbedtls/PSA — checkpoint 1)
 *   2. unwrap the COSE_Encrypt CEK with the device KEK (A128KW)
 *   3. stream ciphertext through psa_aead_update in small chunks
 *   4. verify the GCM tag (checkpoint 2)
 *   5. pipe the decrypted (still-compressed) bytes through libsumo's
 *      streaming zstd decompressor (checkpoint 3)
 *   6. compare expanded output with expected plaintext.
 *
 * The fixture plaintext is 1888 bytes ("Sumo RP2350 checkpoint 3 …" ×32),
 * compressed with zstd, then encrypted with AES-128-GCM. End-to-end this
 * proves the full receive-path: signature → key unwrap → AEAD →
 * decompression, all streaming with O(1) RAM in the image size.
 *
 * Build the fixture set with:
 *
 *   sumo-tool build --signing-key sign.key \
 *     --component "ecu-a,firmware" --seq 3 \
 *     --vendor fa6b4a53d5ad5fdfbe9de663e4d41ffe \
 *     --class  1492af1425695e48bf429b2d51f2ab45 \
 *     --uri "file:///fw" --firmware fw.bin \
 *     --compress --zstd-window-log 10 \
 *     --encrypt devkey.cose \
 *     --payload-output fw.enc \
 *     --output c3.suit
 *
 * The on-device decoder is configured via SUMO_DECOMPRESSOR_WINDOW_LOG_MAX
 * (in this example's CMakeLists.txt) to the same windowLog. The cap is a
 * safety net — frames whose header demands a larger window are rejected
 * before zstd lazy-allocates a heap buffer that would OOM-panic the MCU.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "psa/crypto.h"

#include "sumo/validator.h"
#include "sumo/decompressor.h"

#include "sumo/decryptor_psa.h"

#ifndef DIAG_LED_PIN
#  ifdef PICO_DEFAULT_LED_PIN
#    define DIAG_LED_PIN PICO_DEFAULT_LED_PIN
#  else
#    define DIAG_LED_PIN 25
#  endif
#endif

static void led_init(void)
{
    gpio_init(DIAG_LED_PIN);
    gpio_set_dir(DIAG_LED_PIN, GPIO_OUT);
    gpio_put(DIAG_LED_PIN, 0);
}

static void led_set(int on) { gpio_put(DIAG_LED_PIN, on ? 1 : 0); }

static void blink_n(int n, int on_ms, int off_ms)
{
    for (int i = 0; i < n; i++) {
        led_set(1); sleep_ms(on_ms);
        led_set(0); sleep_ms(off_ms);
    }
}

#define STAGE(n, msg) do {                              \
    printf("stage %d: %s\n", (n), (msg));               \
    fflush(stdout);                                     \
    sleep_ms(50);                                       \
    blink_n((n), 80, 80);                               \
    sleep_ms(200);                                      \
} while (0)

/* --- Embedded fixtures -------------------------------------------- */

static const uint8_t kTrustAnchor[] = {
    0xa5, 0x01, 0x02, 0x03, 0x26, 0x20, 0x01, 0x21, 0x58, 0x20, 0x2e, 0x72,
    0xf7, 0x48, 0xe7, 0x4f, 0x9a, 0x17, 0xee, 0x0b, 0x1d, 0xd7, 0x8c, 0x0e,
    0x89, 0xcf, 0x9f, 0x1b, 0x6b, 0x97, 0x89, 0xa3, 0xad, 0x81, 0x66, 0x7e,
    0x12, 0xe1, 0x9f, 0xfd, 0x22, 0x7a, 0x22, 0x58, 0x20, 0xd0, 0x93, 0xa3,
    0xd3, 0xe8, 0x3d, 0xcb, 0xd1, 0x07, 0x07, 0x0d, 0x05, 0x01, 0x77, 0x92,
    0x07, 0x17, 0x13, 0x76, 0x6e, 0xda, 0xcc, 0x2b, 0xf3, 0xa6, 0xa7, 0xb2,
    0x95, 0x5d, 0x51, 0x7b, 0x82,
};

static const uint8_t kEnvelope[] = {
    0xa2, 0x02, 0x58, 0x73, 0x82, 0x58, 0x24, 0x82, 0x2f, 0x58, 0x20, 0x1b,
    0x57, 0x27, 0x8b, 0x68, 0xc6, 0xae, 0x8e, 0x9f, 0xcf, 0x56, 0x47, 0x36,
    0xbb, 0xb6, 0x64, 0xa1, 0x3b, 0xf1, 0x8a, 0xa8, 0x4f, 0x61, 0xfd, 0xeb,
    0x97, 0xab, 0xfc, 0xa6, 0xc9, 0x31, 0x5c, 0x58, 0x4a, 0xd2, 0x84, 0x43,
    0xa1, 0x01, 0x26, 0xa0, 0xf6, 0x58, 0x40, 0x62, 0xce, 0x1c, 0x38, 0x42,
    0xf8, 0xb1, 0xb3, 0x0e, 0x22, 0x78, 0x27, 0xd1, 0xab, 0xdb, 0xe4, 0x9a,
    0xb5, 0x56, 0x15, 0x77, 0xc0, 0x1b, 0xaa, 0x60, 0x53, 0xba, 0x35, 0x75,
    0x9d, 0x2b, 0x52, 0xdd, 0x31, 0x3c, 0xd4, 0xb5, 0xc6, 0xee, 0x71, 0xaa,
    0xe5, 0xb2, 0x1e, 0xb9, 0xa7, 0xa6, 0x56, 0xfc, 0x48, 0x62, 0x73, 0x18,
    0xa1, 0x74, 0xb9, 0x08, 0x09, 0x57, 0x35, 0x96, 0xe3, 0x82, 0xf2, 0x03,
    0x58, 0xd8, 0xa6, 0x01, 0x01, 0x02, 0x03, 0x03, 0x58, 0xc1, 0xa2, 0x02,
    0x81, 0x82, 0x45, 0x65, 0x63, 0x75, 0x2d, 0x61, 0x48, 0x66, 0x69, 0x72,
    0x6d, 0x77, 0x61, 0x72, 0x65, 0x04, 0x58, 0xab, 0x82, 0x14, 0xa6, 0x01,
    0x50, 0xfa, 0x6b, 0x4a, 0x53, 0xd5, 0xad, 0x5f, 0xdf, 0xbe, 0x9d, 0xe6,
    0x63, 0xe4, 0xd4, 0x1f, 0xfe, 0x02, 0x50, 0x14, 0x92, 0xaf, 0x14, 0x25,
    0x69, 0x5e, 0x48, 0xbf, 0x42, 0x9b, 0x2d, 0x51, 0xf2, 0xab, 0x45, 0x03,
    0x58, 0x24, 0x82, 0x2f, 0x58, 0x20, 0xd1, 0xe0, 0x91, 0x1f, 0xc9, 0xa1,
    0x35, 0x01, 0x83, 0x59, 0xff, 0x95, 0x59, 0xb0, 0x9a, 0x84, 0x62, 0x2f,
    0x28, 0x8c, 0x3b, 0xad, 0x31, 0x7a, 0xea, 0x20, 0x9d, 0x98, 0x04, 0xe0,
    0xc7, 0xfe, 0x0e, 0x19, 0x07, 0x60, 0x13, 0x58, 0x4b, 0x84, 0x43, 0xa1,
    0x01, 0x01, 0xa1, 0x05, 0x4c, 0x07, 0x4e, 0xe1, 0x8e, 0x79, 0x35, 0xf9,
    0x1a, 0xcf, 0x9b, 0x02, 0xfe, 0xf6, 0x81, 0x83, 0x40, 0xa2, 0x01, 0x22,
    0x04, 0x54, 0x2f, 0x74, 0x6d, 0x70, 0x2f, 0x74, 0x31, 0x32, 0x2f, 0x64,
    0x65, 0x76, 0x6b, 0x65, 0x79, 0x2e, 0x63, 0x6f, 0x73, 0x65, 0x58, 0x18,
    0xa5, 0x63, 0x37, 0x7b, 0x92, 0x46, 0x73, 0xd4, 0xfb, 0x9c, 0x37, 0xd5,
    0xbb, 0x2a, 0x29, 0x72, 0x75, 0x53, 0xce, 0x96, 0x7f, 0x9c, 0x85, 0x50,
    0x15, 0x69, 0x23, 0x66, 0x69, 0x72, 0x6d, 0x77, 0x61, 0x72, 0x65, 0x07,
    0x43, 0x82, 0x03, 0x00, 0x09, 0x43, 0x82, 0x17, 0x00, 0x14, 0x43, 0x82,
    0x16, 0x00,
};

static const uint8_t kEncInfo[] = {
    0x84, 0x43, 0xa1, 0x01, 0x01, 0xa1, 0x05, 0x4c, 0x07, 0x4e, 0xe1, 0x8e,
    0x79, 0x35, 0xf9, 0x1a, 0xcf, 0x9b, 0x02, 0xfe, 0xf6, 0x81, 0x83, 0x40,
    0xa2, 0x01, 0x22, 0x04, 0x54, 0x2f, 0x74, 0x6d, 0x70, 0x2f, 0x74, 0x31,
    0x32, 0x2f, 0x64, 0x65, 0x76, 0x6b, 0x65, 0x79, 0x2e, 0x63, 0x6f, 0x73,
    0x65, 0x58, 0x18, 0xa5, 0x63, 0x37, 0x7b, 0x92, 0x46, 0x73, 0xd4, 0xfb,
    0x9c, 0x37, 0xd5, 0xbb, 0x2a, 0x29, 0x72, 0x75, 0x53, 0xce, 0x96, 0x7f,
    0x9c, 0x85, 0x50,
};

static const uint8_t kCiphertext[] = {
    0x01, 0xbe, 0xcd, 0xb5, 0x89, 0x80, 0xff, 0xc5, 0x15, 0xb3, 0x7b, 0x37,
    0x2a, 0xe0, 0xa2, 0xb5, 0x93, 0x92, 0xb8, 0x43, 0x77, 0x0e, 0xb4, 0x44,
    0x7c, 0xff, 0xba, 0xdd, 0x7c, 0xf4, 0x7e, 0x78, 0x43, 0x42, 0xfd, 0x03,
    0xc4, 0x13, 0xad, 0x1c, 0x2b, 0xcf, 0x9f, 0xb9, 0x07, 0xc3, 0xf2, 0x4d,
    0xe5, 0xdc, 0xb5, 0x78, 0x7b, 0x6f, 0x5b, 0xf6, 0x24, 0x30, 0x32, 0x74,
    0x10, 0x5b, 0x31, 0xea, 0xdc, 0x40, 0x16, 0x18, 0x3b, 0x54, 0x5c, 0x90,
    0x81, 0x96, 0xbe, 0xf6, 0x00, 0xe3, 0xa6, 0x6c, 0xda, 0x07, 0xbd, 0x11,
    0xe1, 0x70, 0xf4, 0x97, 0xab, 0x0b, 0x2a, 0x7e, 0x40, 0xe8, 0x03, 0xaa,
    0x4d, 0x03, 0xe8, 0xa6, 0xfc, 0x64, 0x61, 0xd2,
};

static const uint8_t kExpectedPt[] = {
    0x53, 0x75, 0x6d, 0x6f, 0x20, 0x52, 0x50, 0x32, 0x33, 0x35, 0x30, 0x20,
    0x63, 0x68, 0x65, 0x63, 0x6b, 0x70, 0x6f, 0x69, 0x6e, 0x74, 0x20, 0x33,
    0x20, 0xe2, 0x80, 0x94, 0x20, 0x73, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x69,
    0x6e, 0x67, 0x20, 0x64, 0x65, 0x63, 0x6f, 0x6d, 0x70, 0x72, 0x65, 0x73,
    0x73, 0x69, 0x6f, 0x6e, 0x20, 0x74, 0x65, 0x73, 0x74, 0x2e, 0x20,
    /* The fixture is the 59-byte phrase
     *   "Sumo RP2350 checkpoint 3 — streaming decompression test. "
     * (note the U+2014 em-dash and trailing space) repeated 32×, total
     * 1888 bytes — repetitive enough that zstd compresses it to 77 B
     * (then +16 GCM tag = 93 B on the wire). We compare the expanded
     * output stream-of-bytes-style against the expected text below. */
};

/* Same 59-byte phrase, used as the reference for byte-by-byte compare
 * by repeating the prefix kExpectedPt above; the 32 copies + final
 * shorter tail (none — 1888 = 59×32) gives total 1888. */
#define EXPECTED_BLOCK_LEN 59
#define EXPECTED_REPEATS   32
#define EXPECTED_TOTAL_LEN ((size_t)(EXPECTED_BLOCK_LEN * EXPECTED_REPEATS))

/* The 16-byte symmetric KEK that protects the CEK. */
static const uint8_t kKek[16] = {
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
};

/* --- Decompressor sink -------------------------------------------- */

static uint8_t  expanded[2048];
static size_t   expanded_total;

/* Pump (p,n) into zstd, accumulating output into expanded[]. Continues
 * until input is fully consumed AND no further output is produced —
 * which handles the case where zstd has buffered output we still need
 * to drain. */
static int feed_decompressor(sumo_decompressor_t *zd,
                             const uint8_t *p, size_t n)
{
    for (;;) {
        size_t in_len  = n;
        size_t out_cap = sizeof(expanded) - expanded_total;
        if (out_cap == 0) return -1;        /* would overflow sink */

        if (sumo_decompressor_update(zd, p, &in_len,
                                     expanded + expanded_total,
                                     &out_cap) != 0)
            return -1;

        p              += in_len;
        n              -= in_len;
        expanded_total += out_cap;

        if (n == 0 && out_cap == 0) return 0;  /* drained */
    }
}

/* --- App entry ----------------------------------------------------- */

static void hex_dump_n(const char *prefix, const uint8_t *p, size_t n)
{
    printf("%s", prefix);
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
}

int main(void)
{
    led_init();
    blink_n(2, 60, 60);

    stdio_init_all();
    sleep_ms(2000);

    printf("\n--- sumo-rp2350 validate "
           "(checkpoint 3: + decompress) ---\n");
    fflush(stdout);

    STAGE(1, "stdio + led ok");

    /* === Crypto init =============================================== */
    STAGE(2, "calling psa_crypto_init");
    psa_status_t ps = psa_crypto_init();
    if (ps != PSA_SUCCESS) {
        printf("psa_crypto_init FAILED status=%d\n", (int)ps);
        fflush(stdout);
        while (1) blink_n(1, 30, 970);
    }
    STAGE(3, "psa_crypto_init ok");

    /* === Validate envelope ========================================= */
    STAGE(4, "creating validator");
    sumo_validator_t *v = sumo_validator_create(
        kTrustAnchor, sizeof(kTrustAnchor), NULL);
    if (!v) {
        printf("validator_create FAILED\n"); fflush(stdout);
        while (1) blink_n(2, 30, 970);
    }

    STAGE(5, "validating envelope");
    sumo_manifest_t *m = NULL;
    int rc = sumo_validate_envelope(v, kEnvelope, sizeof(kEnvelope), 0, &m);
    printf("validate: rc=%d\n", rc); fflush(stdout);
    if (rc != 0) {
        printf("aborting — manifest signature did not verify\n");
        fflush(stdout);
        while (1) blink_n(3, 30, 970);
    }
    printf("  sequence=%llu  components=%zu\n",
           (unsigned long long)sumo_manifest_sequence_number(m),
           sumo_manifest_component_count(m));
    fflush(stdout);
    STAGE(6, "envelope validated");

    /* === Decryptor + decompressor in series ======================= */
    STAGE(7, "creating decryptor (parse + A128KW unwrap)");
    psa_decryptor_t *d = psa_decryptor_create(
        kEncInfo, sizeof(kEncInfo), kKek, sizeof(kKek));
    if (!d) {
        printf("decryptor_create FAILED\n"); fflush(stdout);
        while (1) blink_n(4, 30, 970);
    }
    STAGE(8, "decryptor created");

    sumo_decompressor_t *zd = sumo_decompressor_create();
    if (!zd) {
        printf("decompressor_create FAILED\n"); fflush(stdout);
        while (1) blink_n(5, 30, 970);
    }
    STAGE(9, "decompressor created");

    /* Stream ciphertext through decrypt → decompress in 24 B chunks.
     * 24 B is below the 16 B GCM tag size so the decryptor's tail-
     * buffering path gets exercised; the decompressor sees variable-
     * size bursts of decrypted bytes. */
    uint8_t       pt_chunk[64];
    const size_t  CHUNK = 24;

    for (size_t i = 0; i < sizeof(kCiphertext); i += CHUNK) {
        size_t n = sizeof(kCiphertext) - i;
        if (n > CHUNK) n = CHUNK;
        size_t got = sizeof(pt_chunk);
        if (psa_decryptor_update(d, kCiphertext + i, n,
                                 pt_chunk, &got) != 0) {
            printf("decryptor_update FAILED at offset %zu\n", i);
            fflush(stdout);
            while (1) blink_n(6, 30, 970);
        }
        if (got > 0 && feed_decompressor(zd, pt_chunk, got) != 0) {
            printf("decompressor_update FAILED at offset %zu\n", i);
            fflush(stdout);
            while (1) blink_n(7, 30, 970);
        }
    }
    STAGE(10, "ciphertext streamed");

    /* Drain decryptor (verifies GCM tag and emits any remaining PT). */
    size_t tail = sizeof(pt_chunk);
    if (psa_decryptor_finalize(d, pt_chunk, &tail) != 0) {
        printf("decryptor_finalize FAILED — GCM tag mismatch\n");
        fflush(stdout);
        while (1) blink_n(8, 30, 970);
    }
    if (tail > 0 && feed_decompressor(zd, pt_chunk, tail) != 0) {
        printf("decompressor_update FAILED on tail bytes\n");
        fflush(stdout);
        while (1) blink_n(9, 30, 970);
    }
    STAGE(11, "decryption complete; tag verified");

    if (sumo_decompressor_finalize(zd) != 0) {
        printf("decompressor_finalize FAILED — frame truncated\n");
        fflush(stdout);
        while (1) blink_n(10, 30, 970);
    }
    printf("decompressed %zu bytes\n", expanded_total);
    fflush(stdout);
    STAGE(12, "decompression complete");

    /* === Cross-check expanded output =============================== */
    int ok = 1;
    if (expanded_total != EXPECTED_TOTAL_LEN) {
        printf("size mismatch: got %zu expected %zu\n",
               expanded_total, EXPECTED_TOTAL_LEN);
        ok = 0;
    } else {
        for (size_t i = 0; i < EXPECTED_REPEATS && ok; i++) {
            if (memcmp(expanded + i * EXPECTED_BLOCK_LEN,
                       kExpectedPt, EXPECTED_BLOCK_LEN) != 0) {
                printf("plaintext mismatch at repeat %zu\n", i);
                hex_dump_n("  got:      ",
                           expanded + i * EXPECTED_BLOCK_LEN,
                           EXPECTED_BLOCK_LEN);
                hex_dump_n("  expected: ", kExpectedPt,
                           EXPECTED_BLOCK_LEN);
                ok = 0;
            }
        }
    }
    if (ok) printf("plaintext matches expected (1888 B = 59 B × 32) ✓\n");
    fflush(stdout);

    sumo_decompressor_free(zd);
    psa_decryptor_free(d);
    sumo_manifest_free(m);
    sumo_validator_free(v);

    printf("idle. (steady 1 Hz blink from here)\n");
    fflush(stdout);
    while (1) {
        led_set(1); sleep_ms(500);
        led_set(0); sleep_ms(500);
    }
    return 0;
}

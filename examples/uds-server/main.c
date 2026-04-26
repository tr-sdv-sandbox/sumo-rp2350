/**
 * @file examples/uds-server/main.c
 * @brief Checkpoint-4b: SUIT-over-UDS download on RP2350.
 *
 * UDS service set:
 *   0x10  DiagSessionControl   (default / programming / extended)
 *   0x11  ECUReset             (hardReset → watchdog_reboot)
 *   0x22  ReadDID              (0xF187 0xF195 0xF18C; 0xF200/F201/F202 dynamic)
 *   0x34  RequestDownload      (begins OTA — accepts size, allocates staging)
 *   0x36  TransferData         (state machine: header → envelope → payload)
 *   0x37  TransferExit         (validate + decrypt + decompress + sha256)
 *   0x3E  TesterPresent        (with suppress-positive-response)
 *
 * Hybrid staging:
 *   - Envelope (manifest + COSE_Sign1) buffers in a 4 KB RAM array.
 *   - Encrypted+compressed payload streams to the inactive flash slot
 *     via platform_rp2350.c's write_fn (offset=0 erases, page-by-page
 *     thereafter).
 *   - On TransferExit:
 *        sumo_validate_envelope(...)                      [signature]
 *        sumo_manifest_encryption_info(...)               [COSE_Encrypt bytes]
 *        psa_decryptor_create + stream-decrypt while reading
 *          ciphertext from XIP-mapped inactive slot
 *        sumo_decompressor_* on-the-fly into a 2 KB RAM plaintext buf
 *        psa_hash_compute(SHA-256, ...) on the plaintext
 *
 * Status DIDs for the host to poll:
 *   0xF200  one ASCII byte: state name (I/H/E/P/V/K/X)
 *   0xF201  32-byte SHA-256 of recovered plaintext (zeros until OK)
 *   0xF202  uint32 LE bytes-consumed (live progress)
 *
 * No A/B activate or reset — those land in 4c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "psa/crypto.h"

#include "uds/uds_server.h"
#include "uds/uds_session.h"
#include "uds/uds_callbacks.h"
#include "isotp/isotp.h"
#include "uds_tiny/can_hw.h"
#include "store/did_store.h"

#include "sumo/validator.h"
#include "sumo/decompressor.h"
#include "sumo/decryptor_psa.h"
#include "sumo/platform_rp2350.h"

#include "app_config.h"
#include "pin_config.h"

/* ── Diag-LED ─────────────────────────────────────────────────────── */
#ifndef DIAG_LED_PIN
#  ifdef PICO_DEFAULT_LED_PIN
#    define DIAG_LED_PIN PICO_DEFAULT_LED_PIN
#  else
#    define DIAG_LED_PIN 25
#  endif
#endif

static void led_init(void) { gpio_init(DIAG_LED_PIN); gpio_set_dir(DIAG_LED_PIN, GPIO_OUT); }
static void led_set(int on) { gpio_put(DIAG_LED_PIN, on ? 1 : 0); }
static void blink_n(int n, int on_ms, int off_ms) {
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

/* ── ISO-TP CAN-ID pair (29-bit, normal-fixed addressing) ────────── */

#define ISOTP_RX_ID (0x18DA0000U \
                     | ((uint32_t)CAN_ECU_ADDR_DEFAULT << 8) \
                     | CAN_TESTER_ADDR)
#define ISOTP_TX_ID (0x18DA0000U \
                     | ((uint32_t)CAN_TESTER_ADDR << 8) \
                     | CAN_ECU_ADDR_DEFAULT)
#define ISOTP_FUNC_RX_ID 0x18DB33F1U

static isotp_channel_t s_phys;

static bool isotp_tx_cb(const can_frame_t *f, void *user) {
    (void)user;
    return can_hw_send(f);
}

/* ── SUIT trust anchor + KEK (must match the host signing key /
 *    devkey.cose used by ota.py to build the fixture envelope). The
 *    same constants are used by examples/validate. */

static const uint8_t kTrustAnchor[] = {
    0xa5, 0x01, 0x02, 0x03, 0x26, 0x20, 0x01, 0x21, 0x58, 0x20, 0x2e, 0x72,
    0xf7, 0x48, 0xe7, 0x4f, 0x9a, 0x17, 0xee, 0x0b, 0x1d, 0xd7, 0x8c, 0x0e,
    0x89, 0xcf, 0x9f, 0x1b, 0x6b, 0x97, 0x89, 0xa3, 0xad, 0x81, 0x66, 0x7e,
    0x12, 0xe1, 0x9f, 0xfd, 0x22, 0x7a, 0x22, 0x58, 0x20, 0xd0, 0x93, 0xa3,
    0xd3, 0xe8, 0x3d, 0xcb, 0xd1, 0x07, 0x07, 0x0d, 0x05, 0x01, 0x77, 0x92,
    0x07, 0x17, 0x13, 0x76, 0x6e, 0xda, 0xcc, 0x2b, 0xf3, 0xa6, 0xa7, 0xb2,
    0x95, 0x5d, 0x51, 0x7b, 0x82,
};

static const uint8_t kKek[16] = {
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
};

/* ── Flash layout (matches examples/minimal). 4 MB Waveshare board:
 *    slot A at  0x100000 (1 MB)
 *    slot B at  0x200000 (1 MB)
 *    fs (kv) at 0x3F0000 (16 KB) */
static const sumo_rp2350_config_t kFlashCfg = {
    .fs_offset     = 0x003F0000,
    .fs_size       = 0x00004000,
    .slot_a_offset = 0x00100000,
    .slot_a_size   = 0x00100000,
    .slot_b_offset = 0x00200000,
    .slot_b_size   = 0x00100000,
    .fetch_fn      = NULL,        /* not used in 4b */
    .fetch_ctx     = NULL,
};

static sumo_validator_t   *s_validator;
static sumo_rp2350_t      *s_platform;
static sumo_platform_ops_t *s_pops;

/* ── OTA state machine ───────────────────────────────────────────── */

#define ENV_BUF_CAP    4096       /* envelope RAM staging cap */
#define PT_BUF_CAP     2048       /* plaintext RAM staging cap (4b only) */
#define PAGE_SIZE      256        /* matches FLASH_PAGE_SIZE */

typedef enum {
    OTA_IDLE = 0,
    OTA_NEED_HEADER,
    OTA_NEED_ENVELOPE,
    OTA_NEED_PAYLOAD,
    OTA_VALIDATING,
    OTA_OK,
    OTA_FAILED,
} ota_state_t;

static ota_state_t s_ota_state;
static const char  s_ota_state_chars[] = "IHEPVKX";

static uint32_t s_ota_total;        /* declared in RequestDownload */
static uint32_t s_ota_consumed;     /* running count of stream bytes */
static uint8_t  s_hdr_buf[4];
static uint8_t  s_hdr_filled;
static uint32_t s_env_len;
static uint8_t  s_env_buf[ENV_BUF_CAP];
static uint32_t s_env_filled;
static uint32_t s_payload_len;
static uint32_t s_payload_offset;   /* into inactive flash slot */
static uint8_t  s_page_buf[PAGE_SIZE];
static uint32_t s_page_filled;

static uint8_t  s_pt_sha256[32];
static uint32_t s_pt_total;

static void ota_reset(void) {
    s_ota_state = OTA_IDLE;
    s_ota_total = s_ota_consumed = 0;
    s_hdr_filled = 0;
    s_env_len = s_env_filled = 0;
    s_payload_len = s_payload_offset = 0;
    s_page_filled = 0;
    memset(s_pt_sha256, 0, sizeof(s_pt_sha256));
    s_pt_total = 0;
}

/* Flush the page buffer into flash at s_payload_offset. The platform
 * write op pads partial pages to FLASH_PAGE_SIZE with 0xFF, so a tail
 * flush at TransferExit time is fine. */
static int flush_page(void) {
    if (s_page_filled == 0) return 0;
    int rc = s_pops->write(NULL, 0, s_payload_offset,
                           s_page_buf, s_page_filled, s_pops->user_ctx);
    if (rc != 0) return rc;
    s_payload_offset += s_page_filled;
    s_page_filled = 0;
    return 0;
}

/* ── UDS OTA callbacks ───────────────────────────────────────────── */

static bool ota_request_cb(uint32_t addr, uint32_t size, uint8_t fmt,
                           const uds_request_t *req, uds_response_t *resp) {
    (void)addr; (void)fmt; (void)req;

    /* Refuse if a transfer is already in flight. */
    if (s_ota_state != OTA_IDLE && s_ota_state != OTA_OK &&
        s_ota_state != OTA_FAILED) {
        resp->len = 2;
        resp->data[0] = 0x7F;       /* negative response */
        resp->data[1] = 0x22;       /* conditionsNotCorrect */
        return false;
    }

    /* Total = 4 (env_len) + envelope + payload. Envelope must fit in
     * RAM staging; payload size capped by inactive slot size. */
    if (size < 4 + 1 + 1 ||
        size > 4 + ENV_BUF_CAP + kFlashCfg.slot_a_size) {
        resp->len = 2;
        resp->data[0] = 0x7F;
        resp->data[1] = 0x31;       /* requestOutOfRange */
        return false;
    }

    ota_reset();
    s_ota_state = OTA_NEED_HEADER;
    s_ota_total = size;

    printf("OTA RequestDownload: %u bytes total\n", (unsigned)size);
    fflush(stdout);

    /* Positive response: max-block-len-format byte + 2-byte max block.
     * lib/uds wants us to fill resp ourselves for app-managed
     * transfers. Format byte 0x20 = "max block length is 2 bytes". */
    resp->len = 4;
    resp->data[0] = 0x74;           /* RequestDownload positive resp */
    resp->data[1] = 0x20;
    resp->data[2] = 0x04;           /* max block length 0x0400 = 1024 */
    resp->data[3] = 0x00;
    return true;
}

static bool ota_data_cb(const uint8_t *data, uint16_t len, uint8_t seq,
                        const uds_request_t *req, uds_response_t *resp) {
    (void)seq; (void)req;

    if (s_ota_state == OTA_IDLE || s_ota_state == OTA_VALIDATING ||
        s_ota_state == OTA_OK   || s_ota_state == OTA_FAILED) {
        resp->len = 2;
        resp->data[0] = 0x7F;
        resp->data[1] = 0x24;       /* requestSequenceError */
        return false;
    }

    while (len > 0) {
        uint16_t step = 0;

        switch (s_ota_state) {
        case OTA_NEED_HEADER: {
            uint16_t want = 4 - s_hdr_filled;
            step = (len < want) ? len : want;
            memcpy(s_hdr_buf + s_hdr_filled, data, step);
            s_hdr_filled += step;
            if (s_hdr_filled == 4) {
                s_env_len = (uint32_t)s_hdr_buf[0]
                          | ((uint32_t)s_hdr_buf[1] << 8)
                          | ((uint32_t)s_hdr_buf[2] << 16)
                          | ((uint32_t)s_hdr_buf[3] << 24);
                if (s_env_len == 0 || s_env_len > ENV_BUF_CAP ||
                    4 + s_env_len > s_ota_total) {
                    s_ota_state = OTA_FAILED;
                    resp->len = 2;
                    resp->data[0] = 0x7F;
                    resp->data[1] = 0x31;  /* requestOutOfRange */
                    return false;
                }
                s_payload_len = s_ota_total - 4 - s_env_len;
                if (s_payload_len > kFlashCfg.slot_a_size) {
                    s_ota_state = OTA_FAILED;
                    resp->len = 2;
                    resp->data[0] = 0x7F;
                    resp->data[1] = 0x31;
                    return false;
                }
                s_ota_state = OTA_NEED_ENVELOPE;
                printf("  env_len=%u  payload_len=%u\n",
                       (unsigned)s_env_len, (unsigned)s_payload_len);
                fflush(stdout);
            }
            break;
        }

        case OTA_NEED_ENVELOPE: {
            uint16_t want = s_env_len - s_env_filled;
            step = (len < want) ? len : want;
            memcpy(s_env_buf + s_env_filled, data, step);
            s_env_filled += step;
            if (s_env_filled == s_env_len) {
                s_ota_state = OTA_NEED_PAYLOAD;
            }
            break;
        }

        case OTA_NEED_PAYLOAD: {
            /* Append into page buf; flush full pages to flash. */
            uint16_t want = PAGE_SIZE - s_page_filled;
            step = (len < want) ? len : want;
            memcpy(s_page_buf + s_page_filled, data, step);
            s_page_filled += step;
            if (s_page_filled == PAGE_SIZE) {
                if (flush_page() != 0) {
                    s_ota_state = OTA_FAILED;
                    resp->len = 2;
                    resp->data[0] = 0x7F;
                    resp->data[1] = 0x72;   /* generalProgrammingFailure */
                    return false;
                }
            }
            break;
        }

        default:
            /* not reachable given the entry guard */
            return false;
        }

        data += step;
        len  -= step;
        s_ota_consumed += step;
    }

    /* Positive response — empty data, just SID + 0x40 + seq counter
     * (lib/uds fills the seq for us). */
    resp->len = 2;
    resp->data[0] = 0x76;            /* TransferData positive resp */
    resp->data[1] = seq;
    return true;
}

/* ── TransferExit pipeline ───────────────────────────────────────── */

static int run_pipeline(void) {
    /* 1. Validate envelope. */
    sumo_manifest_t *m = NULL;
    int rc = sumo_validate_envelope(s_validator, s_env_buf, s_env_filled,
                                     0, &m);
    if (rc != 0) {
        printf("  validate FAILED rc=%d\n", rc);
        return -1;
    }
    printf("  validate OK seq=%llu components=%zu\n",
           (unsigned long long)sumo_manifest_sequence_number(m),
           sumo_manifest_component_count(m));

    /* 2. Pull encryption_info out of the validated manifest. */
    const uint8_t *enc_info; size_t enc_info_len;
    rc = sumo_manifest_encryption_info(m, 0, &enc_info, &enc_info_len);
    if (rc != 0) {
        printf("  no encryption_info on component 0\n");
        sumo_manifest_free(m);
        return -1;
    }

    /* 3. Streaming decrypt → decompress. Read ciphertext directly
     *    from XIP-mapped inactive slot. */
    psa_decryptor_t *d = psa_decryptor_create(enc_info, enc_info_len,
                                               kKek, sizeof(kKek));
    if (!d) {
        printf("  psa_decryptor_create FAILED\n");
        sumo_manifest_free(m);
        return -1;
    }

    sumo_decompressor_t *zd = sumo_decompressor_create();
    if (!zd) {
        printf("  decompressor_create FAILED\n");
        psa_decryptor_free(d); sumo_manifest_free(m);
        return -1;
    }

    /* The inactive slot is XIP-mapped at XIP_BASE + offset. */
    uint8_t s = (uint8_t)sumo_rp2350_active_slot(s_platform);
    uint32_t slot_offset = (s == 0) ? kFlashCfg.slot_b_offset
                                    : kFlashCfg.slot_a_offset;
    const uint8_t *ct = (const uint8_t *)(XIP_BASE + slot_offset);

    static uint8_t pt_buf[PT_BUF_CAP];
    s_pt_total = 0;

    /* Walk the ciphertext in 64 B chunks; pipe each decrypted batch
     * through the decompressor. */
    uint8_t  decrypted[128];
    const uint32_t CHUNK = 64;
    int err = 0;
    for (uint32_t off = 0; off < s_payload_len && !err; off += CHUNK) {
        uint32_t take = s_payload_len - off;
        if (take > CHUNK) take = CHUNK;

        size_t got = sizeof(decrypted);
        if (psa_decryptor_update(d, ct + off, take, decrypted, &got) != 0) {
            err = 1; break;
        }
        if (got == 0) continue;

        const uint8_t *dp = decrypted;
        size_t in_len = got;
        while (in_len > 0) {
            size_t out_cap = PT_BUF_CAP - s_pt_total;
            if (out_cap == 0) { err = 1; break; }
            size_t in_step = in_len;
            int drc = sumo_decompressor_update(zd, dp, &in_step,
                                                pt_buf + s_pt_total,
                                                &out_cap);
            if (drc != 0) { err = 1; break; }
            dp     += in_step;
            in_len -= in_step;
            s_pt_total += out_cap;
            if (in_step == 0 && out_cap == 0) break;
        }
    }

    if (!err) {
        size_t got = sizeof(decrypted);
        if (psa_decryptor_finalize(d, decrypted, &got) != 0) {
            err = 1;
        } else if (got > 0 && s_pt_total + got <= PT_BUF_CAP) {
            size_t in = got, out = PT_BUF_CAP - s_pt_total;
            if (sumo_decompressor_update(zd, decrypted, &in,
                                          pt_buf + s_pt_total, &out) != 0)
                err = 1;
            else
                s_pt_total += out;
        }
        if (!err && sumo_decompressor_finalize(zd) != 0) err = 1;
    }

    psa_decryptor_free(d);
    sumo_decompressor_free(zd);
    sumo_manifest_free(m);

    if (err) { printf("  decrypt/decompress FAILED\n"); return -1; }
    printf("  recovered %u plaintext bytes\n", (unsigned)s_pt_total);

    /* 4. SHA-256 of plaintext for host-side verification. */
    size_t hash_len = 0;
    psa_status_t ps = psa_hash_compute(PSA_ALG_SHA_256,
                                       pt_buf, s_pt_total,
                                       s_pt_sha256, sizeof(s_pt_sha256),
                                       &hash_len);
    if (ps != PSA_SUCCESS || hash_len != 32) {
        printf("  psa_hash_compute FAILED ps=%d\n", (int)ps);
        return -1;
    }
    printf("  sha256: ");
    for (int i = 0; i < 32; i++) printf("%02x", s_pt_sha256[i]);
    printf("\n");
    fflush(stdout);
    return 0;
}

static bool ota_exit_cb(const uds_request_t *req, uds_response_t *resp) {
    (void)req;

    if (s_ota_state != OTA_NEED_PAYLOAD) {
        resp->len = 2;
        resp->data[0] = 0x7F;
        resp->data[1] = 0x24;       /* requestSequenceError */
        return false;
    }
    /* Last partial page → flash. */
    if (flush_page() != 0) {
        s_ota_state = OTA_FAILED;
        resp->len = 2;
        resp->data[0] = 0x7F;
        resp->data[1] = 0x72;
        return false;
    }
    if (s_payload_offset != s_payload_len) {
        s_ota_state = OTA_FAILED;
        resp->len = 2;
        resp->data[0] = 0x7F;
        resp->data[1] = 0x24;
        return false;
    }
    if (s_ota_consumed != s_ota_total) {
        s_ota_state = OTA_FAILED;
        resp->len = 2;
        resp->data[0] = 0x7F;
        resp->data[1] = 0x24;
        return false;
    }

    s_ota_state = OTA_VALIDATING;
    /* Run the SUIT pipeline synchronously. The host's request_timeout
     * is configured generously (>2 s) so signature verify, decrypt,
     * decompress, and SHA-256 all finish well within the window. */
    int rc = run_pipeline();
    s_ota_state = (rc == 0) ? OTA_OK : OTA_FAILED;

    resp->len = 1;
    resp->data[0] = 0x77;            /* TransferExit positive response */
    return true;
}

/* ── DID handlers ────────────────────────────────────────────────── */

static bool ota_did_read(uint16_t did,
                         uint8_t *out, uint8_t *out_len,
                         uint8_t max_len, uint8_t *out_nrc) {
    switch (did) {
    case 0xF200:  /* OTA state */
        if (max_len < 1) { *out_nrc = 0x14; return false; /* responseTooLong */ }
        out[0] = (uint8_t)s_ota_state_chars[s_ota_state];
        *out_len = 1;
        return true;
    case 0xF201:  /* SHA-256 of plaintext */
        if (max_len < 32) { *out_nrc = 0x14; return false; }
        memcpy(out, s_pt_sha256, 32);
        *out_len = 32;
        return true;
    case 0xF202:  /* bytes consumed (uint32 LE) */
        if (max_len < 4) { *out_nrc = 0x14; return false; }
        out[0] = (uint8_t)(s_ota_consumed >>  0);
        out[1] = (uint8_t)(s_ota_consumed >>  8);
        out[2] = (uint8_t)(s_ota_consumed >> 16);
        out[3] = (uint8_t)(s_ota_consumed >> 24);
        *out_len = 4;
        return true;
    default:
        return false;   /* lib/uds will then return a NRC */
    }
}

/* ── ECUReset hook ───────────────────────────────────────────────── */

static void ecu_reset_hook(uint8_t sub_function) {
    (void)sub_function;
    sleep_ms(50);
    watchdog_reboot(0, 0, 0);
    while (1) tight_loop_contents();
}

/* ── App config (registered with uds_server_init) ────────────────── */

static const uds_app_config_t s_app_cfg = {
    .ecu_reset_hook   = ecu_reset_hook,
    .did_read_hook    = ota_did_read,
    .transfer_request = ota_request_cb,
    .transfer_data    = ota_data_cb,
    .transfer_exit    = ota_exit_cb,
};

/* ── Static DID seeding ──────────────────────────────────────────── */

static void seed_did_store(void) {
    did_store_init();

    static const char *spare_part = "sumo-rp2350-checkpoint-4b";
    did_store_add(0xF187, (const uint8_t *)spare_part,
                  (uint8_t)strlen(spare_part), false, DID_ACCESS_PUBLIC);

    static const char *sw_version = "0.1.0-suit-over-uds";
    did_store_add(0xF195, (const uint8_t *)sw_version,
                  (uint8_t)strlen(sw_version), false, DID_ACCESS_PUBLIC);

    static const char *vendor = "tr-sdv-sandbox";
    did_store_add(0xF18C, (const uint8_t *)vendor,
                  (uint8_t)strlen(vendor), false, DID_ACCESS_PUBLIC);
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void) {
    led_init();
    blink_n(2, 60, 60);

    stdio_init_all();
    sleep_ms(2000);

    printf("\n--- sumo-rp2350 uds-server "
           "(checkpoint 4b: SUIT-over-UDS) ---\n");
    fflush(stdout);

    STAGE(1, "stdio + led ok");

    psa_status_t ps = psa_crypto_init();
    if (ps != PSA_SUCCESS) {
        printf("psa_crypto_init FAILED status=%d\n", (int)ps);
        fflush(stdout);
        while (1) blink_n(1, 30, 970);
    }
    STAGE(2, "psa_crypto_init ok");

    if (!can_hw_init()) {
        printf("can_hw_init FAILED — check XL2515 wiring + pin map\n");
        fflush(stdout);
        while (1) blink_n(2, 30, 970);
    }
    STAGE(3, "XL2515 init ok @ 500 kbps, 8 MHz xtal");

    s_platform = sumo_rp2350_create(&kFlashCfg);
    if (!s_platform) {
        printf("sumo_rp2350_create FAILED\n");
        fflush(stdout);
        while (1) blink_n(3, 30, 970);
    }
    s_pops = sumo_rp2350_platform_ops(s_platform);

    s_validator = sumo_validator_create(kTrustAnchor, sizeof(kTrustAnchor),
                                         NULL);
    if (!s_validator) {
        printf("validator_create FAILED\n");
        fflush(stdout);
        while (1) blink_n(4, 30, 970);
    }
    STAGE(4, "platform + validator ready");

    isotp_init(&s_phys, ISOTP_RX_ID, ISOTP_TX_ID, isotp_tx_cb, NULL);
    seed_did_store();
    uds_server_init(&s_app_cfg);

    /* Re-register 0x34/0x36/0x37 with requires_security=false. The lib
     * defaults gate them on SecurityAccess unlock — production-correct,
     * but 4b is testing the OTA pipeline shape, not access control.
     * Adding security back is a 4c (or production hardening) pass.
     *
     * find_service in uds_server.c walks newest-first, so a re-register
     * of an existing SID transparently overrides. We're using the lib's
     * own service handlers (forward-declared below) so the only thing
     * changing is the gate bit. */
    extern void svc_request_download(const uds_request_t *req,
                                      uds_response_t *resp);
    extern void svc_transfer_data   (const uds_request_t *req,
                                      uds_response_t *resp);
    extern void svc_transfer_exit   (const uds_request_t *req,
                                      uds_response_t *resp);
    uds_service_entry_t e;
    e.sid = 0x34; e.handler = svc_request_download;
    e.session_mask = SESSION_MASK_PROGRAMMING; e.requires_security = false;
    uds_server_register(&e);
    e.sid = 0x36; e.handler = svc_transfer_data;
    e.session_mask = SESSION_MASK_PROGRAMMING | SESSION_MASK_EXTENDED;
    uds_server_register(&e);
    e.sid = 0x37; e.handler = svc_transfer_exit;
    uds_server_register(&e);

    STAGE(5, "ISO-TP + DIDs + UDS server initialised");

    printf("Listening on RX=0x%08x  TX=0x%08x  (29-bit)\n",
           (unsigned)ISOTP_RX_ID, (unsigned)ISOTP_TX_ID);
    fflush(stdout);

    uint32_t last_blink_ms = 0;

    for (;;) {
        can_frame_t rx;
        while (can_hw_receive(&rx)) {
            if (rx.id == ISOTP_RX_ID || rx.id == ISOTP_FUNC_RX_ID) {
                isotp_on_rx(&s_phys, &rx);
            }
        }

        isotp_poll(&s_phys);

        if (isotp_rx_ready(&s_phys)) {
            uint16_t req_len;
            const uint8_t *req = isotp_rx_data(&s_phys, &req_len);

            printf("UDS req: %02x", req[0]);
            for (uint16_t i = 1; i < req_len && i < 4; i++) printf(" %02x", req[i]);
            if (req_len > 4) printf(" …(%u B)", req_len);
            printf("\n");
            fflush(stdout);

            uds_response_t resp;
            bool send = uds_server_process(req, req_len,
                                           /*functional=*/false, &resp);
            isotp_rx_done(&s_phys);

            if (send) {
                printf("UDS rsp: %02x (%u B)\n", resp.data[0], resp.len);
                fflush(stdout);
                isotp_send(&s_phys, resp.data, resp.len);
            }
        }

        uds_server_poll();

        uint32_t now = to_ms_since_boot(get_absolute_time());
        if ((now - last_blink_ms) >= 500) {
            led_set((now / 500) & 1);
            last_blink_ms = now;
        }
    }
    return 0;
}

/**
 * @file examples/uds-server/main.c
 * @brief Checkpoint-4b: SUIT-over-UDS download on RP2350.
 *
 * UDS service set:
 *   0x10  DiagSessionControl   (default / programming / extended)
 *   0x11  ECUReset             (hardReset → watchdog_reboot)
 *   0x22  ReadDID              (0xF187 0xF195 0xF18C; 0xF200/F201/F202 dynamic)
 *   0x31  RoutineControl       (0xFF00 = eraseMemory → erase inactive slot)
 *   0x34  RequestDownload      (begins OTA — accepts size, validates state)
 *   0x36  TransferData         (state machine: header → envelope → payload)
 *   0x37  TransferExit         (finalize stream + sha256)
 *   0x3E  TesterPresent        (with suppress-positive-response)
 *
 * Hybrid streaming OTA flow (request order from host):
 *   1. RoutineControl(start, 0xFF00) — kicks off background erase of
 *      the inactive flash slot, sector-at-a-time in the main loop so
 *      CAN polling stays alive. State goes I → P (preparing).
 *   2. ReadDID(0xF200) loop until state == 'R' (ready).
 *   3. RequestDownload(size) — host announces total framed size.
 *   4. TransferData chunks carrying [4 B env_len][envelope][payload],
 *      streamed through:
 *           ciphertext → psa_decryptor_update
 *                      → sumo_decompressor_update (plaintext)
 *                      → flash_range_program (inactive slot, page-by-page)
 *                      → psa_hash_update (running SHA-256)
 *      Plaintext NEVER buffers in RAM; the inactive slot ends up
 *      holding the final firmware ready for activation in 4c.
 *   5. TransferExit — drain decryptor (verifies GCM tag), drain
 *      decompressor (verifies frame end), flush partial page, finish
 *      hash → 0xF201.
 *   6. ReadDID(0xF200) until 'K' (ok) or 'X' (failed).
 *   7. ReadDID(0xF201) — 32-byte SHA-256 of plaintext for cross-check.
 *
 * Status DIDs:
 *   0xF200  one ASCII byte: I/P/R/H/E/D/V/K/X
 *           idle / preparing / ready / header / envelope / downloading /
 *           validating / ok / failed
 *   0xF201  32-byte SHA-256 of recovered plaintext (zeros until OK)
 *   0xF202  uint32 LE bytes-consumed (live progress)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"        /* rom_get_boot_info, rom_explicit_buy */
#include "pico/stdlib.h"
#include "psa/crypto.h"

#include "uds/uds_server.h"
#include "uds/uds_session.h"
#include "uds/uds_callbacks.h"
#include "uds/uds_types.h"
#include "isotp/isotp.h"
#include "uds_tiny/can_hw.h"
#include "store/did_store.h"
#include "mcp2515.h"

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

/* ── SUIT trust anchor + KEK (must match sign.key / devkey.cose used
 *    by ota.py to build the fixture). Same constants as
 *    examples/validate. */

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

/* ── Flash layout (matches examples/minimal). 4 MB Waveshare board. */
static const sumo_rp2350_config_t kFlashCfg = {
    /* littlefs region: 16 KB at 0x3F0000. The partition table's
     * LittleFS-KV slot reserves 64 KB but littlefs only needs 16 KB
     * for our policy keys + boot counter; reformatting an existing
     * 16 KB volume to 64 KB block_count would re-mount-fail and
     * potentially deadlock if the format step hits an erase that
     * straddles the running image. The extra 48 KB of the slot is
     * just unused. */
    .fs_offset     = 0x003F0000,
    .fs_size       = 0x00004000,
    .slot_a_offset = 0x00100000,
    .slot_a_size   = 0x00100000,
    .slot_b_offset = 0x00200000,
    .slot_b_size   = 0x00100000,
    .fetch_fn      = NULL,
    .fetch_ctx     = NULL,
};

static sumo_validator_t   *s_validator;
static sumo_rp2350_t      *s_platform;

/* ── OTA state machine ───────────────────────────────────────────── */

#define ENV_BUF_CAP        4096        /* envelope RAM staging cap */
#define DECRYPT_SCRATCH    256         /* per-chunk decrypt output */
#define PAGE_SIZE          FLASH_PAGE_SIZE

typedef enum {
    OTA_IDLE = 0,
    OTA_PREPARING,        /* erasing inactive slot in main loop */
    OTA_READY,            /* erase complete, awaiting RequestDownload */
    OTA_NEED_HEADER,
    OTA_NEED_ENVELOPE,
    OTA_NEED_PAYLOAD,
    OTA_VALIDATING,
    OTA_STAGED,           /* envelope+payload validated, awaiting activate */
    OTA_TRIAL,            /* booted into new image, awaiting commit */
    OTA_OK,               /* committed (or never trialed) */
    OTA_FAILED,
} ota_state_t;

/* Indexed by ota_state_t for the 0xF200 DID:
 *   I idle  P preparing  R ready  H header  E envelope  D download
 *   V validating  S staged  T trial  K ok  X failed                  */
static const char s_state_chars[] = "IPRHEDVSTKX";

/* Multi-boot trial: orchestrator must call commit before the
 * counter exceeds this. Each cold boot in PENDING state ticks the
 * counter; expiry triggers self-rollback. Tunable per-fleet via
 * app_config.h; default of 5 covers a typical "drove home, drove
 * to work, never reached the orchestrator" sequence. */
#ifndef MAX_TRIAL_BOOTS
#define MAX_TRIAL_BOOTS 5
#endif

#define TRIAL_NORMAL  0
#define TRIAL_PENDING 1

static volatile ota_state_t s_ota_state;

/* Erase progress (only used while OTA_PREPARING). */
static uint32_t s_prepare_offset;

/* Download progress. */
static uint32_t s_ota_total;          /* total framed bytes from RequestDownload */
static uint32_t s_ota_consumed;
static uint8_t  s_hdr_buf[4];
static uint8_t  s_hdr_filled;
static uint32_t s_env_len;
static uint8_t  s_env_buf[ENV_BUF_CAP];
static uint32_t s_env_filled;
static uint32_t s_payload_len;        /* total ciphertext bytes expected */
static uint32_t s_payload_consumed;   /* ciphertext bytes seen so far */

/* Streaming pipeline state — created on env→payload transition,
 * destroyed on TransferExit / OTA_FAILED. */
static psa_decryptor_t       *s_dec;
static sumo_decompressor_t   *s_zd;
static psa_hash_operation_t   s_hash_op = PSA_HASH_OPERATION_INIT;
static uint32_t               s_pt_written;     /* plaintext bytes flashed */
static uint32_t               s_flash_offset;   /* into inactive slot */
static uint8_t                s_page_buf[PAGE_SIZE];
static uint32_t               s_page_filled;

static uint8_t                s_pt_sha256[32];

/* Last boot type — exposed at DID 0xF204 so the orchestrator can
 * confirm a Flash-Update Boot actually happened (vs the bootrom
 * having silently kept booting the old slot). 'N' = normal,
 * 'F' = flash-update (TBYB), 'B' = BOOTSEL, '?' = unknown. */
static char                   s_last_boot_type = '?';

static void ota_pipeline_free(void) {
    if (s_dec) { psa_decryptor_free(s_dec);  s_dec = NULL; }
    if (s_zd)  { sumo_decompressor_free(s_zd); s_zd = NULL; }
    psa_hash_abort(&s_hash_op);
}

static void ota_reset(void) {
    ota_pipeline_free();
    s_ota_state = OTA_IDLE;
    s_prepare_offset = 0;
    s_ota_total = s_ota_consumed = 0;
    s_hdr_filled = 0;
    s_env_len = s_env_filled = 0;
    s_payload_len = s_payload_consumed = 0;
    s_pt_written = s_flash_offset = 0;
    s_page_filled = 0;
    memset(s_pt_sha256, 0, sizeof(s_pt_sha256));
}

/* ── Flash helpers (we manage erase ourselves; plat_write would erase
 *      the whole slot at offset=0 which blacks out CAN for ~6 s on a
 *      1 MB image). */

static void flash_program_page(uint32_t offset,
                               const uint8_t *data, size_t len) {
    uint32_t base = sumo_rp2350_inactive_offset(s_platform);
    uint32_t saved = save_and_disable_interrupts();
    if (len % PAGE_SIZE) {
        /* Pad partial tail to PAGE_SIZE with 0xFF. */
        uint8_t page[PAGE_SIZE];
        memset(page, 0xFF, PAGE_SIZE);
        memcpy(page, data, len);
        flash_range_program(base + offset, page, PAGE_SIZE);
    } else {
        flash_range_program(base + offset, data, len);
    }
    restore_interrupts(saved);
}

static int flush_page_to_flash(void) {
    if (s_page_filled == 0) return 0;
    flash_program_page(s_flash_offset, s_page_buf, s_page_filled);
    s_flash_offset += s_page_filled;
    s_pt_written  += s_page_filled;
    s_page_filled  = 0;
    return 0;
}

/* ── Streaming pipeline: ciphertext → decrypt → decompress → flash + hash */

static int begin_payload_phase(void) {
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

    const uint8_t *enc_info; size_t enc_info_len;
    if (sumo_manifest_encryption_info(m, 0, &enc_info, &enc_info_len) != 0) {
        printf("  no encryption_info on component 0\n");
        sumo_manifest_free(m);
        return -1;
    }

    s_dec = psa_decryptor_create(enc_info, enc_info_len, kKek, sizeof(kKek));
    sumo_manifest_free(m);
    if (!s_dec) { printf("  psa_decryptor_create FAILED\n"); return -1; }

    s_zd = sumo_decompressor_create();
    if (!s_zd) { printf("  decompressor_create FAILED\n");
                 psa_decryptor_free(s_dec); s_dec = NULL; return -1; }

    s_hash_op = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&s_hash_op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        printf("  psa_hash_setup FAILED\n");
        ota_pipeline_free();
        return -1;
    }

    s_pt_written = s_flash_offset = 0;
    s_page_filled = 0;
    return 0;
}

/* Push `n` bytes of decrypted (still-compressed) output through the
 * decompressor → page buffer → flash + hash. Loops to drain all input
 * and any output zstd has buffered. */
static int pump_decompressed(const uint8_t *p, size_t n) {
    while (n > 0) {
        size_t in_step = n;
        size_t out_cap = PAGE_SIZE - s_page_filled;
        if (out_cap == 0) {
            if (flush_page_to_flash() != 0) return -1;
            out_cap = PAGE_SIZE;
        }
        if (sumo_decompressor_update(s_zd, p, &in_step,
                                      s_page_buf + s_page_filled,
                                      &out_cap) != 0)
            return -1;
        if (out_cap > 0) {
            psa_hash_update(&s_hash_op, s_page_buf + s_page_filled, out_cap);
            s_page_filled += out_cap;
        }
        p += in_step;
        n -= in_step;
        if (in_step == 0 && out_cap == 0) break;
    }
    /* Drain any output zstd has buffered after consuming our input. */
    for (;;) {
        size_t in_step = 0;
        size_t out_cap = PAGE_SIZE - s_page_filled;
        if (out_cap == 0) {
            if (flush_page_to_flash() != 0) return -1;
            out_cap = PAGE_SIZE;
        }
        if (sumo_decompressor_update(s_zd, NULL, &in_step,
                                      s_page_buf + s_page_filled, &out_cap) != 0)
            return -1;
        if (out_cap == 0) break;
        psa_hash_update(&s_hash_op, s_page_buf + s_page_filled, out_cap);
        s_page_filled += out_cap;
    }
    return 0;
}

static int feed_ciphertext(const uint8_t *data, uint16_t len) {
    uint8_t  decrypted[DECRYPT_SCRATCH];
    size_t   decrypted_len = sizeof(decrypted);
    if (psa_decryptor_update(s_dec, data, len, decrypted, &decrypted_len) != 0)
        return -1;
    if (decrypted_len > 0)
        return pump_decompressed(decrypted, decrypted_len);
    return 0;
}

/* ── UDS OTA callbacks ───────────────────────────────────────────── */

static bool ota_request_cb(uint32_t addr, uint32_t size, uint8_t fmt,
                           const uds_request_t *req, uds_response_t *resp) {
    (void)addr; (void)fmt; (void)req;

    /* Strict precondition: must have been prepared (slot erased). */
    if (s_ota_state != OTA_READY) {
        resp->len = 2;
        resp->data[0] = 0x7F;
        resp->data[1] = 0x22;       /* conditionsNotCorrect */
        return false;
    }

    uint32_t slot_size = sumo_rp2350_inactive_size(s_platform);
    if (size < 4 + 1 + 1 || size > 4 + ENV_BUF_CAP + slot_size) {
        resp->len = 2;
        resp->data[0] = 0x7F;
        resp->data[1] = 0x31;       /* requestOutOfRange */
        return false;
    }

    s_ota_state    = OTA_NEED_HEADER;
    s_ota_total    = size;
    s_ota_consumed = 0;
    s_hdr_filled   = 0;
    s_env_filled   = 0;
    s_payload_consumed = 0;

    printf("OTA RequestDownload: %u bytes total\n", (unsigned)size);
    fflush(stdout);

    resp->len = 4;
    resp->data[0] = 0x74;
    resp->data[1] = 0x20;
    /* Max block length 0x0100 = 256 bytes per TransferData chunk.
     * One ISO-TP message = 1 FF + ~37 CFs = ~38 frames. Comfortably
     * within the 256-deep MCP2515 SW RX FIFO and well under what the
     * main loop can drain between bursts. Larger blocks (e.g. 1024)
     * empirically lose CFs at our single-core polled drain rate. */
    resp->data[2] = 0x01;
    resp->data[3] = 0x00;
    return true;
}

static bool ota_data_cb(const uint8_t *data, uint16_t len, uint8_t seq,
                        const uds_request_t *req, uds_response_t *resp) {
    (void)req;

    /* Periodic progress only — printf to USB-CDC is ~ms-scale and
     * would starve the CAN drain loop if we logged every chunk. */
    if (seq <= 3 || (seq & 0x1F) == 0) {
        uint32_t txd = 0, rxd = 0;
        mcp2515_get_drops(&txd, &rxd);
        printf("  ota_data: seq=%u len=%u state=%c "
               "consumed=%u/%u rx_drops=%u\n",
               seq, len, s_state_chars[s_ota_state],
               (unsigned)s_ota_consumed, (unsigned)s_ota_total,
               (unsigned)rxd);
        fflush(stdout);
    }

    if (s_ota_state != OTA_NEED_HEADER && s_ota_state != OTA_NEED_ENVELOPE &&
        s_ota_state != OTA_NEED_PAYLOAD) {
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
                    resp->len = 2; resp->data[0] = 0x7F; resp->data[1] = 0x31;
                    return false;
                }
                s_payload_len = s_ota_total - 4 - s_env_len;
                if (s_payload_len > sumo_rp2350_inactive_size(s_platform)) {
                    s_ota_state = OTA_FAILED;
                    resp->len = 2; resp->data[0] = 0x7F; resp->data[1] = 0x31;
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
                /* Validate now, set up the streaming pipeline. */
                if (begin_payload_phase() != 0) {
                    s_ota_state = OTA_FAILED;
                    resp->len = 2; resp->data[0] = 0x7F; resp->data[1] = 0x24;
                    return false;
                }
                s_ota_state = OTA_NEED_PAYLOAD;
            }
            break;
        }

        case OTA_NEED_PAYLOAD: {
            uint16_t want = s_payload_len - s_payload_consumed;
            step = (len < want) ? len : want;
            if (feed_ciphertext(data, step) != 0) {
                s_ota_state = OTA_FAILED;
                ota_pipeline_free();
                resp->len = 2; resp->data[0] = 0x7F; resp->data[1] = 0x72;
                return false;
            }
            s_payload_consumed += step;
            break;
        }

        default:
            return false;
        }

        data += step;
        len  -= step;
        s_ota_consumed += step;
    }

    resp->len = 2;
    resp->data[0] = 0x76;
    resp->data[1] = seq;
    return true;
}

static int finalize_pipeline(void) {
    /* Drain decryptor (verifies GCM tag). Output is final plaintext-
     * compressed bytes that hadn't been emitted yet. */
    uint8_t  tail[16];
    size_t   tail_len = sizeof(tail);
    if (psa_decryptor_finalize(s_dec, tail, &tail_len) != 0) {
        printf("  GCM tag verify FAILED\n");
        return -1;
    }
    if (tail_len > 0 && pump_decompressed(tail, tail_len) != 0)
        return -1;

    /* Drain decompressor (verifies frame end). */
    if (sumo_decompressor_finalize(s_zd) != 0) {
        printf("  decompressor_finalize FAILED\n");
        return -1;
    }

    /* Final partial page → flash. */
    if (flush_page_to_flash() != 0) return -1;

    /* Finish hash. */
    size_t hash_len = 0;
    if (psa_hash_finish(&s_hash_op, s_pt_sha256,
                         sizeof(s_pt_sha256), &hash_len) != PSA_SUCCESS
        || hash_len != 32) {
        printf("  psa_hash_finish FAILED\n");
        return -1;
    }
    printf("  recovered %u plaintext bytes  sha256: ",
           (unsigned)s_pt_written);
    for (int i = 0; i < 32; i++) printf("%02x", s_pt_sha256[i]);
    printf("\n");
    fflush(stdout);
    return 0;
}

static bool ota_exit_cb(const uds_request_t *req, uds_response_t *resp) {
    (void)req;

    if (s_ota_state != OTA_NEED_PAYLOAD ||
        s_payload_consumed != s_payload_len ||
        s_ota_consumed     != s_ota_total) {
        s_ota_state = OTA_FAILED;
        ota_pipeline_free();
        resp->len = 2; resp->data[0] = 0x7F; resp->data[1] = 0x24;
        return false;
    }

    s_ota_state = OTA_VALIDATING;
    int rc = finalize_pipeline();
    ota_pipeline_free();
    /* On success the image is in the inactive slot but NOT yet
     * activated — orchestrator must explicitly trigger
     * RoutineControl(activate) when it's ready. State `S` (staged)
     * marks the awaiting-activate condition, `K` only after a
     * successful trial+commit. */
    s_ota_state = (rc == 0) ? OTA_STAGED : OTA_FAILED;

    resp->len = 1;
    resp->data[0] = 0x77;
    return true;
}

/* ── 0x31 RoutineControl handler ───────────────────────────────────
 *
 *  0xFF00 (eraseMemory)        — prepare phase, erase inactive slot
 *  0xF001 (vendor: activate)   — STAGED → FUB-reboot to new slot in TBYB
 *  0xF002 (vendor: commit)     — TRIAL → clear trial_state, mark OK
 *  0xF003 (vendor: rollback)   — TRIAL → invalidate own image_def + reboot
 */
#define ROUTINE_ERASE_MEMORY 0xFF00
#define ROUTINE_ACTIVATE     0xF001
#define ROUTINE_COMMIT       0xF002
#define ROUTINE_ROLLBACK     0xF003

/* Self-rollback: erase the first sector of OUR slot's flash so the
 * bootrom can't find a valid IMAGE_DEF here on the next reset and
 * picks the surviving (older) slot instead. Caller must reboot
 * immediately afterwards. */
static void invalidate_own_image_def(void) {
    boot_info_t bi;
    if (!rom_get_boot_info(&bi) || bi.partition < 0) {
        return;  /* unpartitioned — can't self-rollback */
    }
    /* Partition index 0 = App-A (0x100000), 1 = App-B (0x200000). */
    uint32_t flash_off =
        (bi.partition == 0) ? kFlashCfg.slot_a_offset
                            : kFlashCfg.slot_b_offset;
    uint32_t saved = save_and_disable_interrupts();
    flash_range_erase(flash_off, FLASH_SECTOR_SIZE);
    restore_interrupts(saved);
}

/* Set trial_state in littlefs. Used by activate (PENDING) and
 * commit (NORMAL). Returns 0 on success. */
static int trial_state_write(int pending) {
    sumo_storage_ops_t *st = sumo_rp2350_storage_ops(s_platform);
    if (!st) return -1;
    if (st->write_u64("trial_state",
                       pending ? TRIAL_PENDING : TRIAL_NORMAL,
                       st->ctx) != 0) return -1;
    if (pending) {
        if (st->write_u64("trial_boots", 0, st->ctx) != 0) return -1;
    }
    return 0;
}

static void app_routine_control(const uds_request_t *req,
                                uds_response_t *resp) {
    if (req->data_len < 3) {
        resp->len = 3; resp->data[0] = 0x7F;
        resp->data[1] = req->sid; resp->data[2] = 0x13;
        return;
    }
    uint8_t  sub        = req->data[0];
    uint16_t routine_id = ((uint16_t)req->data[1] << 8) | req->data[2];

    if (routine_id != ROUTINE_ERASE_MEMORY &&
        routine_id != ROUTINE_ACTIVATE     &&
        routine_id != ROUTINE_COMMIT       &&
        routine_id != ROUTINE_ROLLBACK) {
        resp->len = 3; resp->data[0] = 0x7F;
        resp->data[1] = req->sid; resp->data[2] = 0x31; /* OOR */
        return;
    }

    /* Build the standard "positive RC response" header before any
     * routine-specific tail. SID 0x71 + sub + 2-byte routine ID. */
    resp->data[0] = 0x71;
    resp->data[1] = sub;
    resp->data[2] = (uint8_t)(routine_id >> 8);
    resp->data[3] = (uint8_t)(routine_id & 0xFF);
    resp->len = 4;

    /* sub 0x03 (requestResults) is uniform: append current state. */
    if (sub == 0x03) {
        resp->data[4] = (uint8_t)s_state_chars[s_ota_state];
        resp->len = 5;
        return;
    }

    if (sub != 0x01) {
        resp->len = 3; resp->data[0] = 0x7F;
        resp->data[1] = req->sid; resp->data[2] = 0x12; /* sub-fn NS */
        return;
    }

    /* sub 0x01 = startRoutine. Per-ID behavior: */
    switch (routine_id) {
    case ROUTINE_ERASE_MEMORY:
        ota_reset();
        s_ota_state      = OTA_PREPARING;
        s_prepare_offset = 0;
        printf("OTA prepare: erasing %u-byte inactive slot\n",
               (unsigned)sumo_rp2350_inactive_size(s_platform));
        fflush(stdout);
        return;

    case ROUTINE_ACTIVATE:
        /* precondition: state == STAGED. Set trial_state=PENDING in
         * littlefs (so the new image's boot path knows it's on
         * trial), then FUB-reboot to the inactive slot. */
        if (s_ota_state != OTA_STAGED) {
            resp->len = 3; resp->data[0] = 0x7F;
            resp->data[1] = req->sid; resp->data[2] = 0x22;
            return;
        }
        if (trial_state_write(1) != 0) {
            resp->len = 3; resp->data[0] = 0x7F;
            resp->data[1] = req->sid; resp->data[2] = 0x72;
            return;
        }
        /* The inactive-slot base is what we just wrote into during
         * TransferData. After we send this response, we reboot via
         * FUB so the bootrom prefers that slot. The 1 s delay gives
         * ISO-TP time to flush the response onto the bus before we
         * yank the cores. */
        printf("activate: FUB to slot at 0x%08x in 1s\n",
               (unsigned)sumo_rp2350_inactive_offset(s_platform));
        fflush(stdout);
        sleep_ms(1000);
        rom_reboot(BOOT_TYPE_FLASH_UPDATE, 0,
                   XIP_BASE + sumo_rp2350_inactive_offset(s_platform), 0);
        /* unreached on success; bootrom resets immediately */
        while (1) tight_loop_contents();

    case ROUTINE_COMMIT:
        /* precondition: state == TRIAL. Clear trial_state. */
        if (s_ota_state != OTA_TRIAL) {
            resp->len = 3; resp->data[0] = 0x7F;
            resp->data[1] = req->sid; resp->data[2] = 0x22;
            return;
        }
        if (trial_state_write(0) != 0) {
            resp->len = 3; resp->data[0] = 0x7F;
            resp->data[1] = req->sid; resp->data[2] = 0x72;
            return;
        }
        s_ota_state = OTA_OK;
        printf("commit: trial -> committed\n");
        fflush(stdout);
        return;

    case ROUTINE_ROLLBACK:
        /* precondition: state == TRIAL. Erase IMAGE_DEF + reboot. */
        if (s_ota_state != OTA_TRIAL) {
            resp->len = 3; resp->data[0] = 0x7F;
            resp->data[1] = req->sid; resp->data[2] = 0x22;
            return;
        }
        printf("rollback: invalidating IMAGE_DEF + reboot in 1s\n");
        fflush(stdout);
        sleep_ms(1000);
        invalidate_own_image_def();
        rom_reboot(BOOT_TYPE_NORMAL, 0, 0, 0);
        while (1) tight_loop_contents();
    }
}

/* ── DID handlers ────────────────────────────────────────────────── */

static bool ota_did_read(uint16_t did,
                         uint8_t *out, uint8_t *out_len,
                         uint8_t max_len, uint8_t *out_nrc) {
    switch (did) {
    case 0xF200:
        if (max_len < 1) { *out_nrc = 0x14; return false; }
        out[0] = (uint8_t)s_state_chars[s_ota_state];
        *out_len = 1;
        return true;
    case 0xF201:
        if (max_len < 32) { *out_nrc = 0x14; return false; }
        memcpy(out, s_pt_sha256, 32);
        *out_len = 32;
        return true;
    case 0xF202:
        if (max_len < 4) { *out_nrc = 0x14; return false; }
        out[0] = (uint8_t)(s_ota_consumed >>  0);
        out[1] = (uint8_t)(s_ota_consumed >>  8);
        out[2] = (uint8_t)(s_ota_consumed >> 16);
        out[3] = (uint8_t)(s_ota_consumed >> 24);
        *out_len = 4;
        return true;
    case 0xF203: { /* trial_boots — uint8 (0..MAX_TRIAL_BOOTS) */
        if (max_len < 1) { *out_nrc = 0x14; return false; }
        sumo_storage_ops_t *st = sumo_rp2350_storage_ops(s_platform);
        uint64_t v = 0;
        if (st) (void)st->read_u64("trial_boots", &v, st->ctx);
        out[0] = (uint8_t)(v & 0xFF);
        *out_len = 1;
        return true;
    }
    case 0xF204:  /* last boot type — N/F/B/? — see s_last_boot_type */
        if (max_len < 1) { *out_nrc = 0x14; return false; }
        out[0] = (uint8_t)s_last_boot_type;
        *out_len = 1;
        return true;
    default:
        return false;
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

static void seed_did_store(void) {
    did_store_init();
    static const char *spare_part = "sumo-rp2350-ota-flow";
    did_store_add(0xF187, (const uint8_t *)spare_part,
                  (uint8_t)strlen(spare_part), false, DID_ACCESS_PUBLIC);
    /* OTA_FW_VERSION_STR is "<major>.<minor>.<patch>", baked from
     * the CMake OTA_VERSION_* cache vars at build time. The same
     * major/minor go into PICO_CRT0_VERSION_MAJOR/MINOR which the
     * bootrom uses for A/B slot ranking. */
    static const char *sw_version = OTA_FW_VERSION_STR;
    did_store_add(0xF195, (const uint8_t *)sw_version,
                  (uint8_t)strlen(sw_version), false, DID_ACCESS_PUBLIC);
    static const char *vendor = "tr-sdv-sandbox";
    did_store_add(0xF18C, (const uint8_t *)vendor,
                  (uint8_t)strlen(vendor), false, DID_ACCESS_PUBLIC);
}

/* ── Erase tick — called once per main-loop iteration ──────────── */

static void ota_prepare_tick(void) {
    if (s_ota_state != OTA_PREPARING) return;

    uint32_t base = sumo_rp2350_inactive_offset(s_platform);
    uint32_t cap  = sumo_rp2350_inactive_size  (s_platform);
    if (s_prepare_offset >= cap) {
        s_ota_state = OTA_READY;
        printf("OTA prepare: ready\n");
        fflush(stdout);
        return;
    }
    /* One sector per loop iteration: ~25 ms with interrupts disabled.
     * The MCP2515 hardware buffers 2 frames during this window; the
     * main loop drains them before the next erase, so well-paced host
     * polling never overflows. */
    uint32_t saved = save_and_disable_interrupts();
    flash_range_erase(base + s_prepare_offset, FLASH_SECTOR_SIZE);
    restore_interrupts(saved);
    s_prepare_offset += FLASH_SECTOR_SIZE;
}

/* ── Main loop ───────────────────────────────────────────────────── */

int main(void) {
    led_init();
    blink_n(2, 60, 60);

    stdio_init_all();
    sleep_ms(2000);

    printf("\n--- sumo-rp2350 ota-flow v%s "
           "(checkpoint 4c: A/B + trial + commit) ---\n",
           OTA_FW_VERSION_STR);
    fflush(stdout);

    STAGE(1, "stdio + led ok");

    /* Tier-1 → tier-2 handoff. If the bootrom flagged this boot as
     * a Flash-Update Boot (TBYB), consume the single-shot trial
     * flag here. We've made it past LED + stdio init — far enough
     * that the bootrom's "image bricks before reaching userspace"
     * safety net would have already missed catching us. From here
     * on, the app-level trial counter (in littlefs) protects
     * against multi-boot trial failures across ignition cycles. */
    {
        boot_info_t bi;
        if (rom_get_boot_info(&bi)) {
            /* Mask off any high-bit flags (chained / signed / etc) and
             * just match the lower bits. */
            uint8_t bt = bi.boot_type & 0x7f;
            switch (bt) {
            case BOOT_TYPE_NORMAL:        s_last_boot_type = 'N'; break;
            case BOOT_TYPE_FLASH_UPDATE:  s_last_boot_type = 'F'; break;
            case BOOT_TYPE_BOOTSEL:       s_last_boot_type = 'B'; break;
            default:                      s_last_boot_type = '?'; break;
            }
            printf("boot_type=%c (raw=0x%02x)\n",
                   s_last_boot_type, bi.boot_type);
            fflush(stdout);
            if (s_last_boot_type == 'F') {
                /* Consume the bootrom's TBYB flag. Without this call,
                 * a power-cycle in trial would auto-revert to the old
                 * slot — too aggressive for fleet OTA where the
                 * orchestrator may take many ignition cycles to
                 * confirm health. The 4 KB scratch buffer is needed
                 * for the bootrom's flag-clearing dance. */
                static uint8_t buy_buf[4096] __attribute__((aligned(4)));
                int rc = rom_explicit_buy(buy_buf, sizeof(buy_buf));
                printf("rom_explicit_buy rc=%d\n", rc);
                fflush(stdout);
            }
        } else {
            printf("rom_get_boot_info failed\n");
            fflush(stdout);
        }
    }

    if (psa_crypto_init() != PSA_SUCCESS) {
        printf("psa_crypto_init FAILED\n"); fflush(stdout);
        while (1) blink_n(1, 30, 970);
    }
    STAGE(2, "psa_crypto_init ok");

    if (!can_hw_init()) {
        printf("can_hw_init FAILED\n"); fflush(stdout);
        while (1) blink_n(2, 30, 970);
    }
    STAGE(3, "XL2515 init ok @ 500 kbps, 8 MHz xtal");

    s_platform = sumo_rp2350_create(&kFlashCfg);
    if (!s_platform) {
        printf("sumo_rp2350_create FAILED\n"); fflush(stdout);
        while (1) blink_n(3, 30, 970);
    }

    /* Sync the platform's `active_slot` byte to whichever partition the
     * bootrom actually picked, so `sumo_rp2350_inactive_offset` returns
     * the OTHER slot for the next OTA's TransferData writes. Without
     * this, OTA cycle 2 would overwrite the running image.
     *
     * Reads bi.partition from the bootrom (set up during stage 1) and
     * writes it as the low byte of the active_slot u64 file
     * (platform_rp2350.c reads only the low byte). */
    {
        sumo_storage_ops_t *st = sumo_rp2350_storage_ops(s_platform);
        boot_info_t bi;
        if (rom_get_boot_info(&bi) && bi.partition >= 0) {
            uint8_t my_part = (uint8_t)bi.partition;
            (void)st->write_u64("active_slot", my_part, st->ctx);
            printf("active_slot synced to bootrom partition %u\n",
                   (unsigned)my_part);
            fflush(stdout);
        }
    }

    /* Tier-2 trial counter. We hold the bootrom's commit (we already
     * called rom_explicit_buy if this was a FUB boot), so the bootrom
     * won't auto-revert. From here, the orchestrator owns the trial
     * window — but if it never calls commit/rollback within
     * MAX_TRIAL_BOOTS cold boots (drove home, drove to work,
     * orchestrator never reachable), we self-rollback. */
    {
        sumo_storage_ops_t *st = sumo_rp2350_storage_ops(s_platform);
        uint64_t trial_state = TRIAL_NORMAL;
        (void)st->read_u64("trial_state", &trial_state, st->ctx);
        if (trial_state == TRIAL_PENDING) {
            uint64_t trial_boots = 0;
            (void)st->read_u64("trial_boots", &trial_boots, st->ctx);
            trial_boots++;
            printf("trial boot %llu / %u (max)\n",
                   (unsigned long long)trial_boots,
                   (unsigned)MAX_TRIAL_BOOTS);
            fflush(stdout);
            if (trial_boots > MAX_TRIAL_BOOTS) {
                printf("trial expired without commit — self-rollback\n");
                fflush(stdout);
                /* Don't bother resetting trial_state in our littlefs;
                 * we're erasing our own image anyway, the new (old)
                 * slot's littlefs state is what matters. */
                invalidate_own_image_def();
                rom_reboot(BOOT_TYPE_NORMAL, 0, 0, 0);
                while (1) tight_loop_contents();
            }
            (void)st->write_u64("trial_boots", trial_boots, st->ctx);
            s_ota_state = OTA_TRIAL;
        }
    }

    s_validator = sumo_validator_create(kTrustAnchor, sizeof(kTrustAnchor),
                                         NULL);
    if (!s_validator) {
        printf("validator_create FAILED\n"); fflush(stdout);
        while (1) blink_n(4, 30, 970);
    }
    STAGE(4, "platform + validator ready");

    isotp_init(&s_phys, ISOTP_RX_ID, ISOTP_TX_ID, isotp_tx_cb, NULL);
    seed_did_store();
    uds_server_init(&s_app_cfg);

    /* Re-register 0x34/0x36/0x37/0x31 with requires_security=false.
     * 4b focuses on the OTA pipeline; 4c adds SecurityAccess back. */
    extern void svc_request_download(const uds_request_t *, uds_response_t *);
    extern void svc_transfer_data   (const uds_request_t *, uds_response_t *);
    extern void svc_transfer_exit   (const uds_request_t *, uds_response_t *);
    uds_service_entry_t e;
    e.session_mask = SESSION_MASK_PROGRAMMING; e.requires_security = false;
    e.sid = 0x34; e.handler = svc_request_download; uds_server_register(&e);
    e.session_mask = SESSION_MASK_PROGRAMMING | SESSION_MASK_EXTENDED;
    e.sid = 0x36; e.handler = svc_transfer_data;    uds_server_register(&e);
    e.sid = 0x37; e.handler = svc_transfer_exit;    uds_server_register(&e);
    e.sid = 0x31; e.handler = app_routine_control;  uds_server_register(&e);

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
            bool send = uds_server_process(req, req_len, false, &resp);
            isotp_rx_done(&s_phys);

            if (send) {
                printf("UDS rsp: %02x (%u B)\n", resp.data[0], resp.len);
                fflush(stdout);
                isotp_send(&s_phys, resp.data, resp.len);
            }
        }

        uds_server_poll();

        /* Drive the slot erase one sector per iteration. Sits AFTER
         * the CAN/UDS dispatch so any pending UDS request gets handled
         * before we burn 25 ms in a flash erase. */
        ota_prepare_tick();

        uint32_t now = to_ms_since_boot(get_absolute_time());
        if ((now - last_blink_ms) >= 500) {
            led_set((now / 500) & 1);
            last_blink_ms = now;
        }
    }
    return 0;
}

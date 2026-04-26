/**
 * @file decryptor_psa.c
 * @brief Streaming AES-128-GCM decryption + A128KW key unwrap, all
 *        through PSA Crypto + mbedtls's NIST_KW. RP2350-targeted.
 *
 * Mirrors the public API of libsumo's decryptor.c but talks to mbedtls
 * via the PSA Crypto API (so we link the same pico_mbedtls subset
 * already in use for ES256 verify) plus mbedtls's legacy NIST_KW for
 * AES-KW (PSA Crypto 1.x doesn't expose a clean key-wrap algorithm).
 *
 * No ECDH-ES path — that's ~+50 KB of crypto. A128KW is enough for the
 * "device with a pre-provisioned KEK" deployment shape this checkpoint
 * is showing off.
 *
 * The CBOR helper `parse_cose_encrypt` is duplicated from libsumo's
 * decryptor.c (one of those things to factor into a shared file later
 * once a second backend lands in libsumo proper).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "decryptor_psa.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "qcbor/qcbor_decode.h"
#include "qcbor/qcbor_encode.h"
#include "qcbor/qcbor_spiffy_decode.h"  /* EnterArray/EnterMap/ExitMap */

#include "mbedtls/nist_kw.h"
#include "psa/crypto.h"

#define CEK_LEN     16
#define GCM_IV_LEN  12
#define GCM_TAG_LEN 16

struct psa_decryptor {
    psa_aead_operation_t op;
    psa_key_id_t         key_id;
    uint8_t              cek[CEK_LEN];
    uint8_t              iv [GCM_IV_LEN];
    uint8_t              tail[GCM_TAG_LEN];
    size_t               tail_len;
    int                  initialized;
};

/* --- COSE_Encrypt CBOR parsing (single A128KW recipient).
 * COSE_Encrypt = [ protected_bstr, unprotected_map, ct_or_null,
 *                  recipients_array ]
 * COSE_recipient = [ protected_bstr, unprotected_map, wrapped_cek_bstr ]
 *
 * Returns 0 on success and writes:
 *   iv_out          (12 bytes, from unprotected hdr label 5)
 *   wrapped_cek_out (caller-buffered, length set in *wrapped_cek_len)
 *   recipient_alg   (label 1 in either recipient hdr; -3 = A128KW)
 *   prot_hdr_*      (the outer protected header bytes — needed verbatim
 *                    for AEAD AAD construction)
 */
static int parse_cose_encrypt(
    const uint8_t *enc_info, size_t enc_info_len,
    uint8_t iv_out[GCM_IV_LEN],
    uint8_t *wrapped_cek_out, size_t *wrapped_cek_len,
    int *recipient_alg_out,
    const uint8_t **prot_hdr_out, size_t *prot_hdr_len_out)
{
    QCBORDecodeContext ctx;
    QCBORItem          item;
    UsefulBufC         enc_buf = {enc_info, enc_info_len};

    QCBORDecode_Init(&ctx, enc_buf, QCBOR_DECODE_MODE_NORMAL);

    /* Top-level COSE_Encrypt array (tagged or untagged). */
    QCBORDecode_PeekNext(&ctx, &item);
    QCBORDecode_EnterArray(&ctx, NULL);

    /* [0] outer protected header — bstr. */
    QCBORDecode_GetNext(&ctx, &item);
    if (item.uDataType == QCBOR_TYPE_BYTE_STRING) {
        *prot_hdr_out = item.val.string.ptr;
        *prot_hdr_len_out = item.val.string.len;
    } else {
        *prot_hdr_out = NULL;
        *prot_hdr_len_out = 0;
    }

    /* [1] outer unprotected header — map; iv at label 5. */
    QCBORDecode_EnterMap(&ctx, NULL);
    int got_iv = 0;
    while (QCBORDecode_GetNext(&ctx, &item) == QCBOR_SUCCESS) {
        if (item.label.int64 == 5 &&
            item.uDataType == QCBOR_TYPE_BYTE_STRING &&
            item.val.string.len == GCM_IV_LEN) {
            memcpy(iv_out, item.val.string.ptr, GCM_IV_LEN);
            got_iv = 1;
        }
    }
    QCBORDecode_ExitMap(&ctx);
    if (!got_iv) return -1;

    /* [2] outer ciphertext — null (detached). */
    QCBORDecode_GetNext(&ctx, &item);

    /* [3] recipients array; first recipient. */
    QCBORDecode_EnterArray(&ctx, NULL);
    QCBORDecode_EnterArray(&ctx, NULL);

    /* recipient[0] protected header — algorithm may be here for ECDH. */
    *recipient_alg_out = 0;
    QCBORDecode_GetNext(&ctx, &item);
    if (item.uDataType == QCBOR_TYPE_BYTE_STRING && item.val.string.len > 0) {
        QCBORDecodeContext prot_ctx;
        QCBORItem          prot_item;
        QCBORDecode_Init(&prot_ctx, item.val.string, QCBOR_DECODE_MODE_NORMAL);
        QCBORDecode_EnterMap(&prot_ctx, NULL);
        while (QCBORDecode_GetNext(&prot_ctx, &prot_item) == QCBOR_SUCCESS) {
            if (prot_item.label.int64 == 1) {
                if (prot_item.uDataType == QCBOR_TYPE_INT64)
                    *recipient_alg_out = (int)prot_item.val.int64;
                else if (prot_item.uDataType == QCBOR_TYPE_UINT64)
                    *recipient_alg_out = (int)prot_item.val.uint64;
            }
        }
    }

    /* recipient[1] unprotected header — alg may be here (A128KW path). */
    QCBORDecode_EnterMap(&ctx, NULL);
    while (QCBORDecode_GetNext(&ctx, &item) == QCBOR_SUCCESS) {
        if (item.label.int64 == 1 && *recipient_alg_out == 0) {
            if (item.uDataType == QCBOR_TYPE_INT64)
                *recipient_alg_out = (int)item.val.int64;
            else if (item.uDataType == QCBOR_TYPE_UINT64)
                *recipient_alg_out = (int)item.val.uint64;
        }
    }
    QCBORDecode_ExitMap(&ctx);

    /* recipient[2] wrapped CEK. */
    QCBORDecode_GetNext(&ctx, &item);
    if (item.uDataType != QCBOR_TYPE_BYTE_STRING || item.val.string.len == 0)
        return -1;
    if (item.val.string.len > *wrapped_cek_len) return -1;
    memcpy(wrapped_cek_out, item.val.string.ptr, item.val.string.len);
    *wrapped_cek_len = item.val.string.len;
    return 0;
}

/* --- A128KW unwrap via mbedtls's NIST_KW. PSA Crypto 1.x doesn't
 * expose key-wrap cleanly so we drop down to the legacy mbedtls API. */
static int unwrap_cek_a128kw(const uint8_t *kek, size_t kek_len,
                             const uint8_t *wrapped, size_t wrapped_len,
                             uint8_t cek_out[CEK_LEN])
{
    if (kek_len != CEK_LEN) return -1;
    if (wrapped_len != CEK_LEN + 8) return -1;

    mbedtls_nist_kw_context kw;
    mbedtls_nist_kw_init(&kw);
    int rc = mbedtls_nist_kw_setkey(&kw, MBEDTLS_CIPHER_ID_AES,
                                    kek, (unsigned int)(kek_len * 8),
                                    /*is_wrap=*/0);
    if (rc != 0) goto out;

    size_t out_len = 0;
    rc = mbedtls_nist_kw_unwrap(&kw, MBEDTLS_KW_MODE_KW,
                                wrapped, wrapped_len,
                                cek_out, &out_len, CEK_LEN);
    if (rc == 0 && out_len != CEK_LEN) rc = -1;
out:
    mbedtls_nist_kw_free(&kw);
    return rc == 0 ? 0 : -1;
}

/* --- Public API ---------------------------------------------------- */

psa_decryptor_t *psa_decryptor_create(
    const uint8_t *enc_info, size_t enc_info_len,
    const uint8_t *kek, size_t kek_len)
{
    if (!enc_info || !kek) return NULL;

    /* Parse COSE_Encrypt. */
    uint8_t iv[GCM_IV_LEN];
    uint8_t wrapped[CEK_LEN + 8];
    size_t  wrapped_len = sizeof(wrapped);
    int     recipient_alg = 0;
    const uint8_t *prot_hdr = NULL;
    size_t  prot_hdr_len = 0;

    if (parse_cose_encrypt(enc_info, enc_info_len, iv, wrapped, &wrapped_len,
                           &recipient_alg, &prot_hdr, &prot_hdr_len) != 0)
        return NULL;
    if (recipient_alg != -3) return NULL;  /* A128KW only */

    /* Unwrap CEK. */
    uint8_t cek[CEK_LEN];
    if (unwrap_cek_a128kw(kek, kek_len, wrapped, wrapped_len, cek) != 0)
        return NULL;

    psa_decryptor_t *d = calloc(1, sizeof(*d));
    if (!d) {
        memset(cek, 0, sizeof(cek));
        return NULL;
    }
    memcpy(d->cek, cek, CEK_LEN);
    memcpy(d->iv,  iv,  GCM_IV_LEN);
    memset(cek, 0, sizeof(cek));

    /* Import CEK as a PSA AES-GCM decryption key. */
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm (&attr, PSA_ALG_GCM);
    psa_set_key_type      (&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits      (&attr, 128);

    if (psa_import_key(&attr, d->cek, CEK_LEN, &d->key_id) != PSA_SUCCESS)
        goto fail;

    d->op = (psa_aead_operation_t)PSA_AEAD_OPERATION_INIT;
    if (psa_aead_decrypt_setup(&d->op, d->key_id, PSA_ALG_GCM) != PSA_SUCCESS)
        goto fail;
    if (psa_aead_set_nonce(&d->op, d->iv, GCM_IV_LEN) != PSA_SUCCESS)
        goto fail;

    /* ── XXX INTEROP WORKAROUND — fix me in two repos! ─────────────
     *
     * Per RFC 9052 §5.3 the AAD for an AES-GCM COSE_Encrypt should be
     * the serialized Enc_structure  [ "Encrypt", protected, external_aad ].
     * The matching `psa_aead_update_ad(d->op, aad, aad_len)` is left
     * commented-out below, ready to switch back on once the producer
     * is fixed.
     *
     * Right now the producer (sumo-rs offboard) encrypts with an
     * EMPTY AAD:
     *
     *   sumo-rs/sumo-offboard/src/encryptor.rs
     *     fn encrypt_firmware:      aes_gcm_encrypt(&cek, &iv, &[], plaintext)
     *     fn encrypt_firmware_ecdh: aes_gcm_encrypt(&cek, &iv, &[], plaintext)
     *
     * So we have to also skip AAD here, otherwise GCM tag verify
     * fails. Fix order when we tackle this:
     *
     *   1. sumo-rs:  build the COSE Enc_structure AAD in
     *      sumo-offboard/src/encryptor.rs (both A128KW and ECDH paths)
     *      and pass it to aes_gcm_encrypt instead of `&[]`. Update the
     *      offboard e2e tests to match.
     *
     *   2. sumo-rs onboard validator: the same Enc_structure AAD has
     *      to be reconstructed when verifying — see
     *      sumo-onboard/src/decryptor.rs.
     *
     *   3. libsumo decryptor.c (host, OpenSSL): build + feed the
     *      Enc_structure AAD via EVP_DecryptUpdate(d->ctx, NULL, ...).
     *      The libsumo orchestrator e2e tests will show whether the
     *      change lines up across producer/consumer.
     *
     *   4. THIS file: re-enable the #if 0 block below (delete the
     *      `#if 0` / `#endif`, drop the `(void)prot_hdr;` line). Then
     *      regenerate the validate fixture with the fixed sumo-tool.
     *
     * This is bug #7 in the sumo-rs interop list (six others were
     * fixed during checkpoint 1; see git log on the sumo-rs repo).
     */
    (void)prot_hdr; (void)prot_hdr_len;
#if 0  /* re-enable when sumo-rs offboard emits Enc_structure AAD */
    uint8_t aad_buf[64];
    UsefulBuf  aad_us = {aad_buf, sizeof(aad_buf)};
    UsefulBufC aad;
    QCBOREncodeContext qenc;
    QCBOREncode_Init(&qenc, aad_us);
    QCBOREncode_OpenArray(&qenc);
    QCBOREncode_AddSZString(&qenc, "Encrypt");
    QCBOREncode_AddBytes(&qenc,
        (UsefulBufC){.ptr = prot_hdr, .len = prot_hdr_len});
    QCBOREncode_AddBytes(&qenc, (UsefulBufC){.ptr = NULL, .len = 0});
    QCBOREncode_CloseArray(&qenc);
    if (QCBOREncode_Finish(&qenc, &aad) != QCBOR_SUCCESS) goto fail;
    if (psa_aead_update_ad(&d->op, aad.ptr, aad.len) != PSA_SUCCESS)
        goto fail;
#endif

    d->initialized = 1;
    return d;

fail:
    psa_aead_abort(&d->op);
    psa_destroy_key(d->key_id);
    memset(d, 0, sizeof(*d));
    free(d);
    return NULL;
}

int psa_decryptor_update(psa_decryptor_t *d,
                         const uint8_t *ct, size_t ct_len,
                         uint8_t *pt, size_t *pt_len)
{
    if (!d || !d->initialized || !ct || !pt || !pt_len) return -1;

    /* Same tail-buffering logic as libsumo's OpenSSL decryptor:
     * the GCM tag is appended to the ciphertext stream, so we always
     * hold back the trailing GCM_TAG_LEN bytes of unfed input. */
    size_t total = d->tail_len + ct_len;
    if (total <= GCM_TAG_LEN) {
        memcpy(d->tail + d->tail_len, ct, ct_len);
        d->tail_len = total;
        *pt_len = 0;
        return 0;
    }

    size_t cap = *pt_len;
    size_t produced = 0;
    size_t need_decrypt = total - GCM_TAG_LEN;

    /* First, drain leftover from the tail. */
    if (d->tail_len > 0) {
        size_t from_tail =
            (need_decrypt < d->tail_len) ? need_decrypt : d->tail_len;
        if (from_tail > 0) {
            size_t outl = 0;
            psa_status_t s = psa_aead_update(&d->op, d->tail, from_tail,
                                             pt + produced, cap - produced,
                                             &outl);
            if (s != PSA_SUCCESS) { *pt_len = 0; return -1; }
            produced += outl;
            need_decrypt -= from_tail;
        }
    }

    /* Then decrypt as much of the new ciphertext as keeps a full tail. */
    size_t from_new = (need_decrypt < ct_len) ? need_decrypt : ct_len;
    if (from_new > 0) {
        size_t outl = 0;
        psa_status_t s = psa_aead_update(&d->op, ct, from_new,
                                         pt + produced, cap - produced,
                                         &outl);
        if (s != PSA_SUCCESS) { *pt_len = 0; return -1; }
        produced += outl;
    }

    /* Refresh the tail to the trailing GCM_TAG_LEN bytes. */
    if (ct_len >= GCM_TAG_LEN) {
        memcpy(d->tail, ct + ct_len - GCM_TAG_LEN, GCM_TAG_LEN);
        d->tail_len = GCM_TAG_LEN;
    } else {
        size_t keep = GCM_TAG_LEN - ct_len;
        memmove(d->tail, d->tail + d->tail_len - keep, keep);
        memcpy(d->tail + keep, ct, ct_len);
        d->tail_len = GCM_TAG_LEN;
    }

    *pt_len = produced;
    return 0;
}

int psa_decryptor_finalize(psa_decryptor_t *d, uint8_t *pt, size_t *pt_len)
{
    if (!d || !d->initialized || !pt_len) return -1;
    if (d->tail_len != GCM_TAG_LEN) { *pt_len = 0; return -1; }

    size_t cap = *pt_len;
    size_t produced = 0;
    psa_status_t s = psa_aead_verify(&d->op, pt, cap, &produced,
                                     d->tail, GCM_TAG_LEN);
    if (s != PSA_SUCCESS) { *pt_len = 0; return -1; }
    *pt_len = produced;
    return 0;
}

void psa_decryptor_free(psa_decryptor_t *d)
{
    if (!d) return;
    if (d->initialized) {
        psa_aead_abort(&d->op);
        psa_destroy_key(d->key_id);
    }
    memset(d, 0, sizeof(*d));
    free(d);
}

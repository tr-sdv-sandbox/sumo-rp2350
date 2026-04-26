/**
 * @file decryptor_psa.h
 * @brief Minimal PSA-Crypto-backed streaming AEAD decryptor for the
 *        validate example. See decryptor_psa.c for design notes.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DECRYPTOR_PSA_H
#define DECRYPTOR_PSA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct psa_decryptor psa_decryptor_t;

/**
 * Parse a COSE_Encrypt CBOR + unwrap the wrapped CEK with a 16-byte
 * KEK (A128KW). Returns NULL on parse failure, alg-not-A128KW, or
 * unwrap failure (wrong KEK = CEK comes out garbage and unwrap's
 * integrity check trips).
 */
psa_decryptor_t *psa_decryptor_create(
    const uint8_t *enc_info, size_t enc_info_len,
    const uint8_t *kek, size_t kek_len);

/**
 * Stream a chunk of ciphertext through. Buffers the trailing 16
 * bytes internally (the COSE-appended GCM tag) so callers can hand
 * over arbitrary-sized chunks; only the bytes from previous chunks
 * (less the rolling 16-byte tail) get authenticated and decrypted.
 *
 * On entry, *pt_len is the capacity of `pt`. On return, *pt_len is
 * how many plaintext bytes were written.
 *
 * Returns 0 on success, -1 on error (PSA failure, output buffer too
 * small for what GCM produced this round).
 */
int psa_decryptor_update(psa_decryptor_t *d,
                         const uint8_t *ct, size_t ct_len,
                         uint8_t *pt, size_t *pt_len);

/**
 * Finalize: hand the held-back 16-byte tail to PSA as the GCM tag,
 * verify, and write any final plaintext. On a tag mismatch this
 * returns -1 (and does NOT write to pt).
 *
 * On entry, *pt_len is the capacity of `pt`. On return, *pt_len is
 * how many plaintext bytes were written (typically 0 since GCM has
 * no padding, but PSA defines an output slot for the tail).
 */
int psa_decryptor_finalize(psa_decryptor_t *d,
                           uint8_t *pt, size_t *pt_len);

void psa_decryptor_free(psa_decryptor_t *d);

#ifdef __cplusplus
}
#endif
#endif /* DECRYPTOR_PSA_H */

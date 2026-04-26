/**
 * psa_crypto_config.h — PSA Crypto API surface for ES256 verify only.
 *
 * mbedtls_config.h opts into MBEDTLS_PSA_CRYPTO_CONFIG, which makes
 * the PSA layer read this file to decide which algorithms to expose.
 * Anything not declared with PSA_WANT_* ends up dead-stripped.
 */
#ifndef PSA_CRYPTO_CONFIG_H
#define PSA_CRYPTO_CONFIG_H

#define PSA_WANT_ALG_ECDSA               1
#define PSA_WANT_ALG_DETERMINISTIC_ECDSA 1   /* t_cose accepts both */
#define PSA_WANT_ALG_SHA_256             1

#define PSA_WANT_ECC_SECP_R1_256         1
#define PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY 1

/* Checkpoint 2 — payload decryption: AES-128 + AES-GCM. */
#define PSA_WANT_ALG_GCM                 1
#define PSA_WANT_KEY_TYPE_AES            1

#endif /* PSA_CRYPTO_CONFIG_H */

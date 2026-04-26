/**
 * mbedtls_config.h — minimal config for sumo-rp2350 manifest validation.
 *
 * Enables exactly what t_cose / libcsuit need to verify a SUIT
 * envelope signed with ES256 (ECDSA P-256 + SHA-256). No TLS, no
 * symmetric crypto, no encrypted-payload helpers — those land in
 * checkpoints 2 & 3 along with their config opt-ins.
 */
#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

/* PSA Crypto: t_cose's PSA adapter goes through this API. */
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_CRYPTO_CONFIG     /* read psa_crypto_config.h too */
#define MBEDTLS_USE_PSA_CRYPTO

/* Hash: SHA-256 only. */
#define MBEDTLS_SHA256_C
#define MBEDTLS_MD_C

/* ECDSA + supporting math. */
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C

/* DRBG + entropy — PSA insists on these even for verify-only paths. */
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_AES_C                /* CTR-DRBG dep */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_NO_PLATFORM_ENTROPY  /* no /dev/random on the MCU */
#define MBEDTLS_ENTROPY_HARDWARE_ALT /* hardware RNG provided in main.c */

/* Platform glue. */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY      /* allow custom malloc/free if set */
#define MBEDTLS_ERROR_C              /* helps debugging during bring-up */

/* Trim everything else aggressively — these are the defaults that
 * MBEDTLS_PSA_CRYPTO_CONFIG will gate, but make sure none of the
 * heavy features sneak in. */
/* (left empty; psa_crypto_config.h is the source of truth for the
 * PSA-side surface, which is what t_cose actually consumes.) */

#endif /* MBEDTLS_CONFIG_H */

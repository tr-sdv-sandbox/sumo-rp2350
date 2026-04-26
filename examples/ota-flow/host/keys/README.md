# Test keys — DO NOT USE IN PRODUCTION

These keys are committed to the repo so the bench-test flow is
reproducible without a separate provisioning step. They are **public
test material**:

- `sign.key`  ECDSA P-256 private key. Anyone with it can forge SUIT
  envelopes that the device will validate.
- `sign.pub`  Matching public key (ASN.1, COSE_Key form). The
  derived trust anchor is baked into `examples/uds-server/main.c`
  and `examples/validate/main.c` as `kTrustAnchor[]`.
- `devkey.cose`  16-byte AES-128 key wrapped as a COSE_Key, used
  as the KEK for SUIT payload encryption (also baked into the
  device firmware as `kKek[]`).

These are the same keypair the validate example's baked-in
fixtures were built with (see `/tmp/t12/` history during early
checkpoint development).

For real OTA deployments you would:

1. Generate fresh keys per fleet (e.g. `sumo-tool keygen`).
2. Burn the trust-anchor SHA-256 hash into the RP2350's `BOOTKEY0`
   OTP slot — see `docs/secure-storage.md`.
3. Provision the per-device KEK into a unique `KEY1` OTP slot at
   factory, derive from CHIPID + master, never embed in firmware.

A commit hook should treat anything that imports this directory
as a test-only signal. Do not copy these files into a production
project.

# RP2350 secure storage — survey and migration plan

Findings captured during checkpoint-3 work. The current
`examples/validate` firmware bakes the SUIT trust anchor and the
device KEK as `const uint8_t[]` arrays in `.text`, and keeps
`sumo_seq` / `sumo_sec_ver` / `active_slot` in littlefs on QSPI. That
is fine as a bring-up shape but is not where production should land.
This doc lists what the silicon actually offers and how each app-side
secret should move on-die. Implementation deferred until the diagnostic
protocol layer (UDS-on-CAN / DoIP) is wired up — at that point the
key-provisioning story (factory burn vs. field re-key) becomes
concrete and we can decide layouts in one pass.

## What RP2350 has on die

### OTP — 8 KB, antifuse, ECC'd
Organised as 64 pages × 16 rows × 24 bits (16 b usable + 8 b ECC).
Pre-allocated regions of interest:

| Region                       | Notes |
|------------------------------|---|
| `BOOTKEY0…3` (4 × 256 b)     | SHA-256 hashes of ECDSA-P256 public keys; bootrom verifies signed images against any matching slot |
| `KEY1…KEY6` + `KEY_VALIDn`   | 6 application-controlled 256-bit symmetric keys, served via the bootrom AES service |
| `CHIPID0…3` (256 b)          | Public per-chip random — fleet-level identifier |
| Private 128 b random row     | Factory-burned, secure-only |
| `DEFAULT_BOOT_VERSION0/1`    | Anti-rollback floor for the bootrom A/B picker (2× redundancy) |
| `CRIT0/CRIT1` (×7 redundancy)| Hard-disable cores, JTAG, glitch arming |
| `BOOT_FLAGS0/1` (×3 redund.) | Boot configuration |
| `PAGEn_LOCK0/1` per page     | R/W permissions, secure-only / bootloader-only flags; one-shot |

### QSPI flash
Not encrypted by hardware — RP2350 has no flash-encryption peripheral.
Anything sensitive that lives on QSPI is in the clear to anyone with
bench access.

### SRAM (520 KB)
Partitioned by the SAU into Secure / NonSecure / NSCallable. Cortex-M33
TrustZone-M is fully wired up; secure-only RAM regions are usable.

### Crypto blocks
- **HW SHA-256** — exposed via pico-sdk's `hardware_sha256`, used by the
  pico-sdk mbedtls binding.
- **TRNG** — ROSC-based, fed into pico-sdk's `mbedtls_hardware_poll`.
- **No HW AES, no HW ECC** — all AES-GCM / AES-KW / ECDSA in the current
  firmware is software (mbedtls / PSA on Cortex-M33).
- **Glitch detectors** (voltage / freq / temp) armable via `BOOT_FLAGS`.

### Bootrom services
Reachable from app via `rom_func_lookup`:
- `OTP_ACCESS` — read/write OTP honouring page locks.
- Signed-boot verification — ROM enforces RBL signature against any of
  the 4 `BOOTKEY` slots before handing control to flash.
- A/B partition picker — picks the active slot using
  `DEFAULT_BOOT_VERSION` as the anti-rollback floor.
- Reboot-to-BOOTSEL / picoboot lockout (set in `CRIT0`).

## How each app-level secret should move

| Secret (today)                          | On-die destination |
|-----------------------------------------|---|
| Trust anchor (`kTrustAnchor[]` in .text)| Hash → `BOOTKEY0`. Bootrom roots trust; the app's SUIT validator either consumes the same anchor (loaded at boot) or chains to a SUIT-specific intermediate signed by the boot key. |
| Device KEK (`kKek[16]` in .text)        | `KEY1` slot. Per-device value derived at factory from `CHIPID + master`. Unwrap CEKs via bootrom AES service so the KEK never enters non-secure code in plaintext. |
| `sumo_seq` (replay counter)             | Stays in littlefs — replay protection is integrity-bound to the validator, not confidentiality. Add a MAC over the kv page if the threat model requires it. |
| `sumo_sec_ver` (anti-rollback floor)    | Boot-level rollback uses `DEFAULT_BOOT_VERSION0/1` directly; app-level remains in littlefs. |
| `active_slot`                           | Drop it from littlefs — the bootrom A/B picker already drives this off OTP `FLASH_PARTITION_SLOT_SIZE` + `DEFAULT_BOOT_VERSION`. |
| Per-device identity (UUID)              | Derived from `CHIPID0…3` rather than baked into firmware. |

## Real gaps and threat-model notes

1. **No flash encryption.** If the threat model includes someone
   unsoldering the QSPI chip, confidentiality of firmware bodies on
   flash is gone. Mitigation is the SUIT-level encryption we already
   apply to payloads — leave `KEY1` to do the CEK unwrap so the
   plaintext never lives on QSPI.
2. **No remote-attestation peripheral.** No TPM-style quote. Manual
   attestation can be assembled from
   `CHIPID + private-random + BOOTKEY hash + signed runtime measurements`,
   but it has to be hand-rolled.
3. **OTP is small (8 KB) and write-once.** Plan layouts carefully —
   especially the symmetric-key slot allocation, since `KEY1…KEY6` is
   only six slots and they are antifuse.
4. **Software-only AES / ECC.** Per-op cost matters at scale; not a
   security gap, but an engineering one.

## Migration plan (deferred — pending UDS/DoIP)

When we revisit this, expected order of operations:
1. Pin a key-provisioning ceremony (factory burn vs. first-boot
   self-keygen vs. reseed-over-UDS) — this picks which OTP slots get
   filled when.
2. Move trust anchor → `BOOTKEY0`. Switch sample firmware to be
   signed (RBL packaging via `picotool sign`).
3. Move device KEK → `KEY1`. Replace `kKek[16]` in
   `decryptor_psa.c` with a bootrom-AES-service unwrap.
4. Drop `active_slot` from littlefs; switch fetch-and-write paths
   to bootrom partition APIs.
5. Wire `DEFAULT_BOOT_VERSION` writes into the policy commit path.
6. Enable glitch detectors + `CRIT0` core-disable for production
   builds (separate config from dev).

The reason for waiting on UDS/DoIP: re-keying and key rotation in the
field is what stresses the OTP layout. Once we know whether the field
protocol carries a "rotate-KEK" command and what reseed semantics
look like, we can lock layouts in one pass instead of reflowing OTP
twice.

# examples/uds-server — checkpoints 4a + 4b

UDS-on-CAN server running on a Waveshare RP2350-CAN. Two layered
demos:

- **4a** — bare UDS bring-up (DiagSession + ECUReset + ReadDID +
  TesterPresent), driven by `host/run-test.sh`.
- **4b** — SUIT-over-UDS download: host pushes a SUIT envelope+payload
  over UDS RequestDownload + TransferData + TransferExit, device
  validates the envelope, decrypts and decompresses the payload, and
  exposes the recovered plaintext's SHA-256 via DID `0xF201`. Driven
  by `host/run-ota.sh`. A/B activate + reset is deferred to 4c.

## What's running on the device

```
   main.c
     │
     ▼
   isotp_init(&ch, RX_ID, TX_ID, can_send_cb, NULL)
     │  ─ register can_send_cb that forwards to mcp2515_send
     │
   uds_server_init(&app_cfg)
     │  ─ register ECUReset hook → watchdog_reboot
     │
   seed_did_store()
     │  ─ 0xF187 (spare part), 0xF195 (sw ver), 0xF18C (vendor)
     │
   for(;;):
     can_hw_receive() ── frames ──▶ isotp_on_rx
     isotp_poll                                         (CF/FC timing)
     isotp_rx_ready ─▶ isotp_rx_data ─▶ uds_server_process
                                                   │
                                            isotp_send ─▶ can_hw_send
     uds_server_poll                              (S3 timer / lockout)
```

CAN: 500 kbit/s, 29-bit extended addressing, 8 MHz crystal on the
XL2515. Device IDs default to ECU=0x42, tester=0xF1, giving:
- physical RX (host→device): `0x18DA42F1`
- physical TX (device→host): `0x18DAF142`
- functional RX:             `0x18DB33F1` (subscribed but not used yet)

## Hardware

Waveshare RP2350-CAN (XL2515 + SIT65HVD230). Pin map in `pin_config.h`:

| Function     | GPIO | Pico-SDK signal |
|--------------|------|-----------------|
| SPI1_SCK     | 10   | spi1            |
| SPI1_MOSI    | 11   | spi1            |
| SPI1_MISO    | 12   | spi1            |
| MCP2515 /CS  | 9    |                 |
| MCP2515 /INT | 8    |                 |

If your board's silkscreen disagrees, edit `pin_config.h` accordingly.

## Build + flash

```
cd sumo-rp2350/examples/uds-server
./flash.sh
```

(`flash.sh` builds, `picotool load -x`'s the UF2, then attaches a
USB-CDC console at 115200 8N1.)

Expected console output:

```
--- sumo-rp2350 uds-server (checkpoint 4a: UDS bring-up) ---
stage 1: stdio + led ok
stage 2: XL2515 init ok @ 500 kbps, 8 MHz xtal
stage 3: ISO-TP channel created
stage 4: DID store seeded (F187 F195 F18C)
stage 5: UDS server initialised
Listening on RX=0x18da42f1  TX=0x18daf142  (29-bit)
```

LED on `PICO_DEFAULT_LED_PIN` then blinks at 1 Hz; each incoming UDS
request prints a `UDS req: ...` / `UDS rsp: ...` line.

## Bench loop with a Linux host

Needs a USB-CAN adapter (slcan-style — CANable v1, generic dongles,
etc.). Wire H↔H, L↔L, plus one termination resistor at the far end
(the Waveshare board has a selectable 120 Ω terminator).

```
cd host
sudo ./setup-can.sh                # auto-picks the non-Pico ttyACM,
                                   # bridges via slcand to can0 @ 500k
./run-test.sh                      # creates host/.venv on first run,
                                   # installs deps, runs tester.py
```

If you have multiple CAN adapters or RP2350 boards plugged in, the
auto-pick will refuse to guess; pass the device explicitly:

```
sudo ./setup-can.sh /dev/ttyACM2 can0
python3 tester.py can0
```

Teardown (kills slcand and brings down all canX):
```
sudo ./setup-can.sh down
```

For gs_usb-class adapters (CANable Pro), the bridging step is
unnecessary — they appear as canX directly. Drop in `ip link set
canX up type can bitrate 500000` instead of the slcand call.

Expected:

```
=== DiagSession(extended) ===
  ← True session_type=3
=== ReadDID(0xF187) ===
  ← 'sumo-rp2350-checkpoint-4a'
=== ReadDID(0xF195) ===
  ← '0.1.0-uds-bringup'
=== ReadDID(0xF18C) ===
  ← 'tr-sdv-sandbox'
=== TesterPresent ×3 (suppress positive) ===
  ← (suppressed)  …  ← (suppressed)  …  ← (suppressed)
=== DiagSession(default) ===
  ← True session_type=1
OK — full UDS round-trip succeeded.
```

## Sizes

| Section | Bytes |
|---|---|
| `.text` | 181,992 |
| `.bss`  | 30,524 |
| `.bin`  | 168 KB  |
| `.uf2`  | 344 KB |

The 4a-only build was 61 KB .text; the +120 KB jump is libsumo +
libcsuit + zstd_dec + decryptor_psa + the mbedtls subset needed for
the SUIT receive path.

Of that, `uds_tiny::uds + ::store + ::isotp` total ~12 KB after
`-Os --gc-sections`. The rest is pico-sdk runtime + USB-CDC + the
MCP2515 driver.

## SUIT-over-UDS (4b)

```
cd host
sudo ./setup-can.sh
./run-ota.sh                       # builds fixture, pushes via UDS
```

Expected output:

```
building fixture envelope (sumo-tool build) ...
  envelope: 388 B  (/tmp/sumo-rp2350-4b/c4b.suit)
  payload:  93 B  (/tmp/sumo-rp2350-4b/fw.enc)
framed: 4 + 388 + 93 = 485 bytes
expected plaintext SHA-256: 0d0c46f9f9fe646eefe9ee5ca416ee28dfb96dabb4e8e6168610beca3a455620
using can0

=== DiagSession(programming) ===     ← session_echo=2
=== RequestDownload(addr=0, size=485) ===     ← max_block=1024
=== TransferData (chunks of 1024) ===         → sent 485/485
=== TransferExit ===                  ← ok
=== polling DID 0xF200 (state) ===   state='K'

device sha256:   0d0c46f9...
expected sha256: 0d0c46f9...

OK — SUIT-over-UDS pipeline verified end-to-end.
```

App-level framing the host script applies before pushing:

```
+----------------+-----------------+-------------------+
| env_len (4 B   | envelope        | payload           |
|  LE uint32)    |  (manifest+sig) | (encrypted+zstd)  |
+----------------+-----------------+-------------------+
```

Device state machine walks bytes through three states —
`NEED_HEADER` (parse 4 B env_len), `NEED_ENVELOPE` (buffer N bytes
into a 4 KB RAM staging area), `NEED_PAYLOAD` (stream to inactive
flash slot via `platform_rp2350.c`'s page-by-page write_fn). At
TransferExit:

1. `sumo_validate_envelope(...)` — ECDSA-P256 manifest signature.
2. `sumo_manifest_encryption_info(...)` — pulls the `COSE_Encrypt`
   bytes out of the validated manifest (libsumo accessor added in
   the same commit).
3. `psa_decryptor_create + update + finalize` — reads ciphertext
   directly from XIP-mapped inactive slot, AES-128-GCM streaming.
4. `sumo_decompressor_*` — zstd into a 2 KB RAM plaintext buffer.
5. `psa_hash_compute(SHA_256, ...)` — exposed at DID `0xF201`.

Status DIDs the host polls:

| DID    | Returns                          |
|--------|----------------------------------|
| 0xF200 | one ASCII byte: state name (`I H E P V K X` for idle/header/env/payload/validating/ok/failed) |
| 0xF201 | 32-byte SHA-256 of plaintext (zeros until OK) |
| 0xF202 | uint32 LE bytes-consumed (live progress)  |

## What's not here yet

- **A/B activate + reset** — bootrom partition picker integration;
  4c.
- **Plaintext-to-flash for real-firmware sizes** — 4b's plaintext
  lands in a 2 KB RAM buffer; works for the ~1.6 KB fixture but won't
  for MB-scale firmware. Fixed in 4c with proper bootloader handoff.
- **SecurityAccess** — RequestDownload / TransferData / TransferExit
  registrations in `main.c` re-register the lib's defaults with
  `requires_security=false` so 4b focuses on the OTA pipeline shape.
  Production wants the lib's default `requires_security=true` plus a
  real seed/key challenge — 4c.
- **Native CAN-FD via PIO** — if/when we want to drop the MCP2515 in
  favour of pure PIO, the `can_hw.c` shim is the only file that
  changes.

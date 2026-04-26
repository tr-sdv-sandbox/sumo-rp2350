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
./run-ota.sh                       # default fixture (~1.5 KB)
./run-ota.sh --reps 21845          # 1 MB plaintext fixture
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

Flow (host's perspective):

1. `RoutineControl(start, 0xFF00 eraseMemory)` — kicks off background
   erase of the inactive flash slot. Device drives one 4 KB sector
   per main-loop iteration so CAN polling stays alive.
2. Poll `ReadDID(0xF200)` until state == `'R'` (ready).
3. `RequestDownload(size)` — host announces total framed size.
4. `TransferData` chunks of 256 bytes carrying
   `[4 B env_len][envelope][payload]` concatenated. Device parses
   bytes through a state machine — `NEED_HEADER` (4 B), 
   `NEED_ENVELOPE` (buffered into a 4 KB RAM array), `NEED_PAYLOAD`
   (streamed through the pipeline below).
5. `TransferExit` — finalize; state goes `V` (validating) → `K`/`X`.
6. `ReadDID(0xF201)` — 32-byte SHA-256 of the recovered plaintext.

Streaming pipeline in `NEED_PAYLOAD` (per chunk):

```
ciphertext bytes
  → psa_decryptor_update          AES-128-GCM, A128KW-unwrapped CEK
  → sumo_decompressor_update      zstd, windowLog 10
  → 256-byte page buffer
  → flash_range_program           inactive slot, page-by-page
  → psa_hash_update               running SHA-256
```

Plaintext **never accumulates in RAM** — it lands directly in the
inactive flash slot, sized for real firmware. `TransferExit` flushes
the partial page, `psa_decryptor_finalize` verifies the GCM tag,
`sumo_decompressor_finalize` verifies the zstd frame ended cleanly,
and `psa_hash_finish` emits the final SHA-256 to DID `0xF201` for
host cross-check.

Why `max_block=256` (not 1024 or larger): one ISO-TP message at
1024 bytes = 1 FF + 146 CFs ≈ 36 ms on the wire at 500 kbit.
Single-core polled drain (`mcp2515_receive` in the main loop) can
keep up with that burst most of the time but loses the occasional
CF, ISO-TP aborts on the resulting SN mismatch, and the host times
out. 256-byte chunks are 38-frame bursts — comfortably within the
drain rate. Dual-core split (CAN/ISO-TP on Core 1 with a SPSC
queue to Core 0) would unlock larger blocks; tracked as a 4c
follow-up if higher OTA throughput becomes useful.

Status DIDs the host polls:

| DID    | Returns                          |
|--------|----------------------------------|
| 0xF200 | one ASCII byte: state name (`I P R H E D V K X` for idle/preparing/ready/header/env/downloading/validating/ok/failed) |
| 0xF201 | 32-byte SHA-256 of plaintext (zeros until OK) |
| 0xF202 | uint32 LE bytes-consumed (live progress)  |

## What's not here yet

- **A/B activate + reset** — bootrom partition picker integration;
  4c.
- **SecurityAccess** — RequestDownload / TransferData / TransferExit
  registrations in `main.c` re-register the lib's defaults with
  `requires_security=false` so 4b focuses on the OTA pipeline shape.
  Production wants the lib's default `requires_security=true` plus a
  real seed/key challenge — 4c.
- **Larger TransferData blocks (1024 / 4096 B)** — needs the CAN/
  ISO-TP loop on Core 1 with a SPSC queue feeding Core 0. The
  RP2040 reference's dual-core split is the existing pattern;
  follow-up for higher OTA throughput.
- **Native CAN-FD via PIO** — if/when we want to drop the MCP2515 in
  favour of pure PIO, the `can_hw.c` shim is the only file that
  changes.

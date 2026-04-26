# examples/ota-flow — checkpoint 4c

A/B OTA on a Waveshare RP2350-CAN with the bootrom's
flash-update-boot + try-before-you-buy mechanism doing the slot
switch and rollback. Builds on 4b's SUIT-over-UDS download path,
adds activate / commit / rollback as RoutineControl IDs, and an
end-to-end orchestrator that runs the device through a chain of
firmware versions.

## Demo

```
$ ./flash-factory.sh                     # one-shot factory install
$ ./host/run-ota-cycle.sh 1.1.0 1.2.0 1.3.0
```

Each version: build → SUIT-package → push over UDS to the inactive
slot → activate → ECUReset → bootrom does FUB+TBYB into the new
slot → 10-second settle → commit → assert `ReadDID(0xF195)` returns
the new version.

## Flow at the wire

```
Tester                                                      Device
──────                                                      ──────
DiagnosticSessionControl(programming) ───────────────────▶  state I
RoutineControl(0xFF00 eraseMemory) ──────────────────────▶  state P
                                              ◀────────── erase tick (~6 s)
ReadDID(0xF200)  ────────────────────────────────────────▶  state R
RequestDownload(0, framed_size) ─────────────────────────▶  R → ready
TransferData(seq=1..N, 256 B chunks) ────────────────────▶  D (writing)
                                              ◀────────── decrypt + zstd
                                                          + flash + sha256
TransferExit ────────────────────────────────────────────▶  V → S
ReadDID(0xF200) ─────────────────────────────────────────▶  S (staged)

RoutineControl(0xF001 activate) ─────────────────────────▶  S (still),
                                                          trial_state=PENDING
                                                          in littlefs
ECUReset(hardReset) ─────────────────────────────────────▶  ecu_reset_hook:
                                                          rom_reboot(FUB,
                                                            XIP+slot_b, …)
                                                  ◀──── chip resets, FUB
                                                  ◀──── bootrom validates
                                                          App-B IMAGE_DEF
                                                          (hash + signature)
                                                  ◀──── App-B boots,
                                                          state = T (trial),
                                                          tbyb_info=0x01
[reconnect ISO-TP] ─────────────────────────────────────▶  Listening …
ReadDID(0xF195) ─────────────────────────────────────────▶  "1.1.0"
ReadDID(0xF200) ─────────────────────────────────────────▶  T
ReadDID(0xF203)  poll until 1 (settle elapsed) ──────────▶  0/1
                                                          (10 s default)

RoutineControl(0xF002 commit) ───────────────────────────▶  rom_explicit_buy
                                                          → bootrom erases
                                                            App-A IMAGE_DEF
                                                          → state = K
ReadDID(0xF200) ─────────────────────────────────────────▶  K (committed)
```

## Design choices and the bootrom side of things

### A/B + TBYB requires a SIGNED IMAGE_DEF, not just hashed

Even on an UNSECURED RP2350 (`secure_boot=0`), the bootrom's
flash-update-boot validation will silently fall back to the prior
slot if the FUB target only carries a HASH_DEF — `boot_type=F`
surfaces but `tbyb_and_update_info` stays zero, `boot_diagnostic`
stays zero, and `partition` is the previous slot. Adding a
`SIGNATURE` block (any secp256k1 PEM key works since the public
key's OTP fingerprint isn't checked when secure boot is off) makes
TBYB actually engage. The pico-examples `picow_ota_update`
reference does the same: `pico_hash_binary` + `pico_sign_binary`.

### Activate is "arm only"; tester drives the reset

In an automotive flow the tester decides when a reset is safe:
the vehicle may be moving, or several ECUs may need to be staged
before any of them resets. So our `RoutineControl(0xF001 activate)`:

- Persists `trial_state = PENDING` and the staged slot's flash
  offset to littlefs.
- Returns the standard positive RoutineControl response.
- **Does not reboot.**

The tester then issues UDS `ECUReset(hardReset)` separately. The
device's `ecu_reset_hook` reads the trial flag and chooses between
a FLASH_UPDATE_BOOT (if armed) and a normal `watchdog_reboot`.

### Commit is gated on a 10-second settle period

The bootrom's TBYB watchdog gives us 16.7 seconds before it auto-
rolls back to the prior slot. We don't want to consume that budget
by committing the very first instant we boot the new image — the
new firmware should at least make it through `main()` initialisation
and run the dispatch loop for a while before we declare it good.

The boot path enters trial without calling `rom_explicit_buy`,
records `s_trial_boot_ms`, and pets the bootrom watchdog every
main-loop iteration. After `TRIAL_SETTLE_MS` (10 s default) of
healthy iteration the device sets `s_trial_settle_passed = true`
and exposes `1` on `DID 0xF203`. Any commit request before then
returns NRC `0x22`. The orchestrator polls `0xF203` so it can fire
the commit immediately when the gate opens, rather than waiting a
full P2 timeout.

### Auto-rollback is in HW, not in our code

Once the new image is in trial, three things can happen:

1. **App keeps running, tester commits** → `rom_explicit_buy`
   succeeds → bootrom erases the OTHER slot's IMAGE_DEF → state = K.
   Subsequent normal boots stay in the new slot. **No app-level
   rollback after this point** — the bootrom A/B picker can no
   longer find the old image.
2. **App hangs (hardfault, infinite loop, …)** → main loop stops
   petting the bootrom watchdog → 16.7 s later watchdog fires →
   chip resets → bootrom on next boot picks the surviving slot
   (TBYB-flagged candidate is skipped on a non-FUB boot). Automatic
   rollback in HW.
3. **Power loss before commit** → bootrom's TBYB pending flag is
   still set → next power-up: bootrom doesn't see FUB intent, picks
   the un-TBYB'd slot (the prior one). Tester sees the device back
   in the old version; can re-issue ECUReset to retry the trial.

The previous design's "littlefs `trial_boots` counter +
`MAX_TRIAL_BOOTS` self-rollback" is gone. It was incompatible with
this flow: once `rom_explicit_buy` runs, the bootrom erases the
other slot, so a later self-rollback by erasing our own IMAGE_DEF
would brick the device. The bootrom's HW watchdog is both simpler
and more reliable.

### Flash layout

```
0x000000 .. 0x0FFFFF (1 MB)   unpartitioned
0x100000 .. 0x1FFFFF (1 MB)   App-A (id 0)
0x200000 .. 0x2FFFFF (1 MB)   App-B (id 0, linked A-pair of A)
0x300000 .. 0x3EFFFF          unpartitioned
0x3F0000 .. 0x3FFFFF (64 KB)  LittleFS-KV (id 1)
```

The two main-app partitions share `id = 0` and B carries
`"link": ["a", 0]` — matching pico-examples' `picow_ota_update`.
The bootrom uses the `link` to identify A/B siblings; the IDs don't
need to be unique.

`unpartitioned` accepts the `absolute` family for the RP2350-E10
erratum fix.

## DIDs

| DID    | Type      | Description                                    |
|--------|-----------|------------------------------------------------|
| 0xF187 | string    | Spare-part identifier (`sumo-rp2350-ota-flow`) |
| 0xF195 | string    | Software version (e.g. `"1.1.0"`)              |
| 0xF200 | char      | OTA state (`I`/`P`/`R`/`H`/`E`/`D`/`V`/`S`/`T`/`K`/`X`) |
| 0xF201 | sha256    | Recovered plaintext SHA-256 (after V state)    |
| 0xF202 | uint32 LE | Bytes consumed in current download             |
| 0xF203 | uint8     | Trial settle elapsed (`0` = no, `1` = yes)     |
| 0xF204 | char      | Last boot type (`N`/`F`/`B`/`?`)               |

## RoutineControl IDs

| ID      | Function    | Effect                                                                      |
|---------|-------------|-----------------------------------------------------------------------------|
| 0xFF00  | eraseMemory | Erase the inactive slot, transitions to state R                             |
| 0xF001  | activate    | Persist `trial_state=PENDING` + target slot offset in littlefs              |
| 0xF002  | commit      | Gated on settle; calls `rom_explicit_buy`; state → K                        |
| 0xF003  | rollback    | (Trial only) Erase OWN IMAGE_DEF + reboot — bootrom picks survivor         |

## Files

```
ota-flow/
├── CMakeLists.txt          # version-stamped build, hash + sign + TBYB
├── README.md               # this file
├── partition_table.json    # A/B + LittleFS layout
├── flash-factory.sh        # one-shot factory install of v1.0.0 + PT
├── test-picotool-fub.sh    # debugging helper for the FUB pipeline
├── test-version-ranking.sh # debugging helper for bootrom A/B ranking
├── pico_sdk_import.cmake
├── pin_config.h            # XL2515 SPI pinout
├── app_config.h            # uds-tiny + MCP2515 sizing knobs
├── mbedtls_config.h        # PSA crypto configuration
├── psa_crypto_config.h
├── main.c                  # application
├── uds_platform_time.c     # uds-tiny time hook
├── hal/
│   ├── can_hw.{c,h}        # uds-tiny CAN-HAL adapter
│   ├── mcp2515.{c,h}       # IRQ-driven driver, 256-deep RX FIFO
│   └── mcp2515_regs.h
├── keys/
│   ├── README.md
│   ├── bootkey.pem         # secp256k1 — RP2350 IMAGE_DEF signing
│   ├── sign.key            # COSE — sumo-tool envelope signing
│   ├── sign.pub
│   └── devkey.cose         # COSE — sumo-tool encryption KEK
└── host/
    ├── log-console.sh      # auto-reconnecting USB-CDC tail
    ├── ota-cycle.py        # multi-version OTA chain orchestrator
    ├── ota.py              # single-cycle helper (legacy)
    ├── run-ota-cycle.sh    # venv launcher
    ├── run-ota.sh
    ├── run-test.sh
    ├── setup-can.sh
    ├── tester.py
    └── requirements.txt
```

## Hardware

Waveshare RP2350-CAN (XL2515 + SIT65HVD230). Pin map in
`pin_config.h`. CAN at 500 kbit/s, 29-bit extended addressing,
8 MHz crystal on the XL2515.

ECU = `0x42`, tester = `0xF1`:
- physical RX (host→device): `0x18DA42F1`
- physical TX (device→host): `0x18DAF142`
- functional RX:             `0x18DB33F1`

## Build + first flash

```sh
# one-shot: cmake builds v1.0.0, partition table is created and
# loaded, factory image is written into App-A, device reboots into
# the application:
./flash-factory.sh

# verify:
picotool info -a -f
# Partition 0  version=1.0  hash: verified  signature: verified
# Partition 1  version=…    (whatever's there from prior runs)
```

## Run an OTA chain

In one terminal:

```sh
./host/log-console.sh -t        # auto-reconnecting console viewer
```

In another:

```sh
./host/run-ota-cycle.sh 1.1.0 1.2.0 1.3.0
```

Each cycle:
1. cmake-builds the firmware at the requested version (TBYB on)
2. Wraps it in a SUIT envelope (sumo-tool)
3. Pushes envelope+payload over UDS RequestDownload+TransferData
4. RoutineControl(activate) — arm trial
5. ECUReset — bootrom FUB into the new slot
6. ReadDID(0xF195) — confirm version bumped
7. Poll 0xF203 — wait for the settle gate to open
8. RoutineControl(commit) — bootrom commits (erases prior slot's
   IMAGE_DEF)

## Known limitations

- **No post-commit rollback**: once `rom_explicit_buy` runs, the
  bootrom erases the OTHER slot's IMAGE_DEF. The device can no
  longer fall back to the prior version through the bootrom's A/B
  picker. To roll back you would have to OTA the prior version
  again — i.e. roll forward to the same content with a higher
  version stamp.
- **No security access**: download / activate / commit are all
  registered with `requires_security=false` for the demo. Adding
  seed-key challenge would be a small addition (uds-tiny supports
  it), but every existing UDS service stays authenticated until
  you do.
- **TBYB watchdog is 16.7 s**: the main loop must remain responsive
  enough to call `watchdog_update()` faster than that. If a future
  feature ever needs to block longer (e.g. a multi-second flash
  erase), it must pet the watchdog itself or split the work across
  multiple iterations the way `ota_prepare_tick` already does.

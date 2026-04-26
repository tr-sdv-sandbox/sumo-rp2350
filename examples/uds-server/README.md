# examples/uds-server — checkpoint 4a

Minimal UDS-on-CAN server running on a Waveshare RP2350-CAN. Goal of
this checkpoint: prove the full CAN → ISO-TP → UDS stack works on
RP2350 against a Linux-host tester. SUIT integration lands in 4b.

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
| `.text` | 59,632 |
| `.bss`  | 11,900 |
| `.bin`  | 53 KB  |
| `.uf2`  | 105 KB |

Of that, `uds_tiny::uds + ::store + ::isotp` total ~12 KB after
`-Os --gc-sections`. The rest is pico-sdk runtime + USB-CDC + the
MCP2515 driver.

## What's not here yet

- **SUIT** — TransferData / TransferExit / RoutineControl callbacks
  routed into libsumo. Lands in checkpoint 4b.
- **A/B activate + reset** — uses the bootrom partition picker;
  arrives with 4c.
- **SecurityAccess** — the lib supports 0x27 with seed/key out of the
  box; no app-side hookup yet because the bench tester doesn't need
  it.
- **Native CAN-FD via PIO** — if/when we want to drop the MCP2515 in
  favour of pure PIO, the `can_hw.c` shim is the only file that
  changes.

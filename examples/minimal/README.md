# sumo-rp2350 — minimal example

Smoke-test app: mounts the sumo kv filesystem on flash, prints the
current `boot_count` (and the libsumo policy slots), increments it,
parks. The point is to prove the binding compiles and runs on real
hardware — not to demonstrate a full OTA flow.

## Build

Run the top-level `setup-deps.sh` once first to install the
cross-toolchain, build picotool, and pin the Pico SDK
(see `../../README.md`). Then:

```sh
source ../../env.sh                     # exports PICO_SDK_PATH
cd examples/minimal
mkdir build && cd build
cmake -DPICO_BOARD=pico2 ..
make -j
picotool load -f sumo_rp2350_minimal.uf2
```

Note: libsumo's onboard 3rdparty deps (libcsuit, t_cose, qcbor,
libzstd, and a crypto backend like mbedTLS) still need to be
cross-built for Cortex-M33 — that's the outstanding bring-up work.
Until then the example links but `sumo_validator_create` etc.
won't have working crypto on-device. The kv layer (mount, read,
write) does work without those.

Open the USB-CDC console (115200 baud, board enumerates as `/dev/ttyACMx`):

```
--- sumo-rp2350 minimal ---
kv mounted, active_slot = 0
first boot — boot_count not yet stored
boot_count now = 1
sumo_seq=(none) 0 sumo_sec_ver=(none) 0 reject_before=(none) 0
idle.
```

Reset the board to confirm the `boot_count` survives across power
cycles — that's the kv working over real flash.

## What this does *not* prove

- That `sumo_process_image` works on-device (no fetch transport here).
- That envelope validation works on-device (depends on the cross-
  built crypto deps mentioned above).
- A/B slot swap (no firmware to swap to yet).

Use this as a beachhead, then layer the UDS/CAN transport sibling +
real envelope validation on top.

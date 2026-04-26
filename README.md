# sumo-rp2350

Pico-SDK platform binding for **libsumo** on Raspberry Pi RP2350-class
boards. Target: Waveshare RP2350-CAN. Built as a CMake `INTERFACE`
library so it compiles inside the consuming firmware app — no
top-level pico-sdk integration here.

## What it provides

A single shared handle that vends both libsumo onboard callback
bundles:

```c
#include <sumo/platform_rp2350.h>

sumo_rp2350_t       *sumo_rp2350_create      (const sumo_rp2350_config_t *cfg);
sumo_storage_ops_t  *sumo_rp2350_storage_ops (sumo_rp2350_t *r);
sumo_platform_ops_t *sumo_rp2350_platform_ops(sumo_rp2350_t *r);
int                  sumo_rp2350_active_slot (sumo_rp2350_t *r);
void                 sumo_rp2350_free        (sumo_rp2350_t *r);
```

- `storage_ops` keeps libsumo policy values (`sumo_seq`, `sumo_sec_ver`,
  `sumo_reject_before`) plus the active-slot byte in a **littlefs**
  filesystem mounted on a configurable flash region. Each key is a
  small file; littlefs handles wear leveling, power-fail recovery,
  and arbitrary key sets.
- `platform_ops` writes decrypted firmware into one of two raw-flash
  staging slots (A/B), bypassing littlefs since firmware images are
  large and want page-direct writes. `swap()` flips
  `/active_slot` in the fs; `invoke()` is a default no-op the
  integrator can override. `fetch()` is **not** implemented here —
  the integrator supplies a function pointer that pulls payloads
  from whatever transport the app uses (UDS-over-CAN-ISO-TP, raw
  CAN, USB-CDC, …).

## What it does *not* do

- **No transport.** UDS / ISO-TP / raw CAN reassembly all live in a
  separate sibling repo (TBD). This binding only exposes a
  `fetch_fn` hook the transport plugs into.
- **No bootloader.** RP2350 doesn't need a custom one for OTA — the
  integrator's app does the validation and re-flashing in user space
  before letting the device reset.

## Building

`sumo-rp2350` is consumed from a Pico-SDK app:

```cmake
# In your firmware's top-level CMakeLists.txt
cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(my_ecu C CXX ASM)
pico_sdk_init()

add_subdirectory(../sumo-workspace/components/libsumo libsumo)
add_subdirectory(../sumo-workspace/components/sumo-rp2350 sumo-rp2350)

add_executable(my_ecu main.c)
target_link_libraries(my_ecu
    pico_stdlib
    hardware_flash
    hardware_sync
    sumo_onboard
    sumo-rp2350)
pico_add_extra_outputs(my_ecu)
```

## Flash layout

Configurable via `sumo_rp2350_config_t`. Defaults assume 4 MB flash:

| Region    | Default offset (XIP-relative) | Size   | Used for                       |
|-----------|-------------------------------|--------|--------------------------------|
| App image | `0x0000_0000`                 | 1 MB   | Bootloader-loaded firmware     |
| Slot A    | `0x0010_0000`                 | 1 MB   | OTA staging A                  |
| Slot B    | `0x0020_0000`                 | 1 MB   | OTA staging B                  |
| sumo fs   | `0x003F_0000`                 | 16 KB  | littlefs (sumo_seq, sec_ver, …)|

The integrator can override any of these; the only invariants are
4 KB sector alignment, non-overlap, and `fs_size >= 8 KB` (littlefs
needs two blocks for its metadata pair).

## Storage backend — littlefs

`storage_ops` is backed by [littlefs](https://github.com/littlefs-project/littlefs)
(vendored at `3rdparty/littlefs`, currently v2.11.3). On first boot
the configured region is auto-formatted; subsequent boots just
mount. Each policy key (`/sumo_seq`, `/sumo_sec_ver`,
`/sumo_reject_before`, `/active_slot`) is a small file. The block
device callbacks bridge littlefs to `flash_range_program` /
`flash_range_erase` under a `save_and_disable_interrupts()` critical
section so the XIP instruction fetch can't fault while a sector is
being programmed.

Why a real fs over a raw struct: arbitrary keys without redesigning
the layout, real wear leveling for environments where the device
sees frequent re-validation, and standard power-fail recovery
behaviour — all for ~10 KB of code.

## Example app

`examples/minimal/` is a tiny smoke-test that mounts the kv,
increments a `boot_count` field, prints state, and parks. Use it as
the first thing you flash on a fresh board to confirm the binding
runs end-to-end. Build instructions live in that directory's README.

## Status

v1 — minimum viable. Storage works for any string key (libsumo's
known three plus anything else the integrator wants). The pieces
that still need attention:

- Cross-built crypto deps (libcsuit / t_cose / qcbor / libzstd /
  OpenSSL or mbedTLS) for Cortex-M33 — the example links but
  validation needs these on-device.
- Multi-component A/B slots (today swap() tracks one global active
  byte; multi-image firmware needs per-component slots).

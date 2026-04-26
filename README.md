# sumo-rp2350

Pico-SDK platform binding for **libsumo** on Raspberry Pi RP2350-class
boards. Target: Waveshare RP2350-CAN. Built as a CMake `INTERFACE`
library so it compiles inside the consuming firmware app — no
top-level pico-sdk integration here.

## What it provides

Two factory functions, both implementing the libsumo onboard
callback contracts:

```c
#include <sumo/platform_rp2350.h>

sumo_storage_ops_t *sumo_rp2350_storage_ops (const sumo_rp2350_config_t *cfg);
sumo_platform_ops_t *sumo_rp2350_platform_ops(const sumo_rp2350_config_t *cfg);
```

- `storage_ops` keeps the libsumo policy (`sumo_seq`, `sumo_sec_ver`,
  `sumo_reject_before`) in a single dedicated 4 KB flash sector,
  written atomically with a CRC32 + magic guard. Strict-greater
  rollback semantics from libsumo handle bad/incomplete writes safely.
- `platform_ops` writes decrypted firmware into one of two staging
  slots (A/B). `swap()` flips an active-slot byte stored in the kv
  sector; `invoke()` is a default no-op the integrator can override.
  `fetch()` is **not** implemented here — the integrator supplies a
  function pointer that pulls payloads from whatever transport the
  app uses (UDS-over-CAN-ISO-TP, raw CAN, USB-CDC, …).

## What it does *not* do

- **No transport.** UDS / ISO-TP / raw CAN reassembly all live in a
  separate sibling repo (TBD). This binding only exposes a
  `fetch_fn` hook the transport plugs into.
- **No bootloader.** RP2350 doesn't need a custom one for OTA — the
  integrator's app does the validation and re-flashing in user space
  before letting the device reset.
- **No filesystem.** v1 uses raw flash with a fixed-layout kv blob.
  pico-littlefs is a clean future swap (the
  `sumo_storage_ops_t` abstraction was designed for it).

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

| Region       | Default offset (XIP-relative) | Size  | Used for                              |
|--------------|-------------------------------|-------|---------------------------------------|
| App image    | `0x0000_0000`                 | 1 MB  | Bootloader-loaded firmware            |
| Slot A       | `0x0010_0000`                 | 1 MB  | OTA staging A                         |
| Slot B       | `0x0020_0000`                 | 1 MB  | OTA staging B                         |
| sumo kv      | `0x003F_F000`                 | 4 KB  | sumo_seq / sumo_sec_ver / active slot |

The integrator can override any of these; the only invariants are
sector-alignment (4 KB) and non-overlap.

## Status

v1 — minimum viable. Storage works for known libsumo keys
(`sumo_seq`, `sumo_sec_ver`, `sumo_reject_before`) and the active-slot
flag. Arbitrary-key support, wear leveling, and pico-littlefs are
deferred until a real device run flags them as needed.

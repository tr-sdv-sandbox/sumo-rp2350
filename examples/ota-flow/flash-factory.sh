#!/usr/bin/env bash
# Factory bring-up for the ota-flow demo.
#
#   1. Build a non-TBYB factory image at the requested version
#      (default 1.0.0).
#   2. Generate a partition table from partition_table.json.
#   3. Reboot the device into BOOTSEL and erase the entire flash so
#      we start from a known clean state.
#   4. Flash the partition table.
#   5. Flash the factory image into App-A.
#   6. Reboot into the application; attach USB-CDC console so the
#      operator can verify it came up.
#
# Re-runnable: every invocation wipes the device and re-installs.
# Subsequent OTA pushes are driven by host/ota-cycle.sh, NOT this
# script.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build-factory"
PT_JSON="$SCRIPT_DIR/partition_table.json"
PT_BIN="$BUILD_DIR/partition_table.uf2"
TARGET="sumo_rp2350_ota-flow"
UF2="$BUILD_DIR/${TARGET}.uf2"

VERSION_MAJOR="${1:-1}"
VERSION_MINOR="${2:-0}"
VERSION_PATCH="${3:-0}"

ENV_FILE="$SCRIPT_DIR/../../env.sh"
if [ -f "$ENV_FILE" ]; then
    # shellcheck source=/dev/null
    . "$ENV_FILE"
fi
if [ -z "${PICO_SDK_PATH:-}" ]; then
    echo "error: PICO_SDK_PATH is not set" >&2
    echo "       run ../../setup-deps.sh first." >&2
    exit 1
fi

# 1. Build factory image (TBYB OFF)
echo "==> Building $TARGET v${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}"
mkdir -p "$BUILD_DIR"
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
      -DPICO_BOARD=pico2 \
      -DCMAKE_BUILD_TYPE=Release \
      -DOTA_VERSION_MAJOR="$VERSION_MAJOR" \
      -DOTA_VERSION_MINOR="$VERSION_MINOR" \
      -DOTA_VERSION_PATCH="$VERSION_PATCH" \
      -DOTA_TBYB=OFF > /dev/null
cmake --build "$BUILD_DIR" -j --target "$TARGET" 2>&1 | tail -3
[ -f "$UF2" ] || { echo "error: $UF2 missing after build" >&2; exit 1; }

# 2. Generate partition table. --abs-block places the PT block at a
# bootrom-discoverable absolute location in flash; without it the
# block is written to the start of flash but `picotool partition
# info` (and presumably the bootrom A/B picker) won't find it.
echo "==> picotool partition create $(basename "$PT_JSON")"
# --abs-block takes an optional <abs_block_loc> hex positional, so any
# flag placed after it gets parsed as that hex value (we hit "--quiet
# is not a valid hex value"). Put --quiet up front and keep
# --abs-block last on this line.
picotool partition create --quiet "$PT_JSON" "$PT_BIN" --abs-block

# 3. Reboot to BOOTSEL + full flash erase. Run reboot loudly so we see
# whether the device was reachable at all — silent failure here is
# what makes the rest of the script appear to "do nothing".
echo "==> picotool reboot -uf  (force into BOOTSEL)"
picotool reboot -uf || {
    echo "warn: reboot -uf failed; if the device is already in BOOTSEL"
    echo "      this is fine — continuing."
}
sleep 3

echo "==> picotool erase  (wipe all 4 MB)"
picotool erase

# 4. Flash partition table. --ignore-partitions overwrites any prior
# PT (otherwise the device's old PT can refuse the write if its
# bootloader perms were 'r'). Erase above already wiped flash, but
# --ignore-partitions stays defensive.
echo "==> picotool load partition_table.uf2"
picotool load -v --ignore-partitions "$PT_BIN"

# Reboot back to BOOTSEL so picotool re-reads the freshly-written
# partition table on its next connect. Without this, picotool's
# cached partition state from before the PT load is stale and the
# next load step errors with "modified data (pt modified since load)".
echo "==> picotool reboot -u  (re-enter BOOTSEL with new PT)"
picotool reboot -u || true
sleep 3

# 5. Flash factory image into App-A. Partition index 0 in our table
# is App-A. picotool's -p takes the *index*, not the id/name.
echo "==> picotool load -p 0 $(basename "$UF2")  (= App-A)"
picotool load -v -p 0 "$UF2"

# 6. Reboot to application
echo "==> picotool reboot -a"
picotool reboot -a || true
sleep 3

# 7. Attach console
SERIAL=""
for _ in $(seq 1 20); do
    for d in /dev/ttyACM*; do
        [ -e "$d" ] || continue
        vid=$(udevadm info -q property "$d" 2>/dev/null \
              | sed -n 's/^ID_VENDOR_ID=//p')
        if [ "$vid" = "2e8a" ]; then SERIAL="$d"; break; fi
    done
    [ -n "$SERIAL" ] && break
    sleep 0.5
done
if [ -z "$SERIAL" ]; then
    echo "warning: no /dev/ttyACMx with VID 2e8a found within 10 s." >&2
    echo "       Check the cable; the board may have flashed OK." >&2
    exit 0
fi

echo "==> attaching $SERIAL @ 115200"
echo "    Ctrl-A then K to detach (screen) / Ctrl-A X (minicom)"
if command -v screen >/dev/null 2>&1; then
    exec screen "$SERIAL" 115200
elif command -v minicom >/dev/null 2>&1; then
    exec minicom -D "$SERIAL" -b 115200
else
    stty -F "$SERIAL" 115200 cs8 -cstopb -parenb raw -echo
    exec cat "$SERIAL"
fi

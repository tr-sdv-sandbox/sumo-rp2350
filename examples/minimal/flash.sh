#!/usr/bin/env bash
# Build, flash, and attach a serial console for the sumo-rp2350
# example whose directory this script lives in.
#
# Generic — works for any examples/<name>/ subdirectory; the CMake
# executable target is assumed to be `sumo_rp2350_<name>` (matches the
# convention in our example CMakeLists.txt files).
#
# Flow:
#   1. source ../../env.sh so PICO_SDK_PATH is set (from setup-deps.sh)
#   2. cmake configure + build the target
#   3. picotool: try to load + execute. If the device isn't in BOOTSEL
#      and we can reach a running RP-series via USB, ask it to reboot
#      into BOOTSEL and retry.
#   4. wait for the new firmware's USB-CDC console to come up, attach.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXAMPLE_NAME="$(basename "$SCRIPT_DIR")"
TARGET="sumo_rp2350_${EXAMPLE_NAME}"
BUILD_DIR="$SCRIPT_DIR/build"
UF2="$BUILD_DIR/${TARGET}.uf2"
BAUD=115200

# ── env.sh from setup-deps.sh: PICO_SDK_PATH ──────────────────────
ENV_FILE="$SCRIPT_DIR/../../env.sh"
if [ -f "$ENV_FILE" ]; then
    # shellcheck source=/dev/null
    . "$ENV_FILE"
fi
if [ -z "${PICO_SDK_PATH:-}" ]; then
    echo "error: PICO_SDK_PATH is not set" >&2
    echo "       run ../../setup-deps.sh first, then re-run this." >&2
    exit 1
fi

# ── Build ─────────────────────────────────────────────────────────
echo "==> Building $TARGET"
mkdir -p "$BUILD_DIR"
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
          -DPICO_BOARD=pico2 \
          -DCMAKE_BUILD_TYPE=Release > /dev/null
fi
cmake --build "$BUILD_DIR" -j --target "$TARGET" 2>&1 | tail -3

if [ ! -f "$UF2" ]; then
    echo "error: $UF2 not found after build" >&2
    exit 1
fi
echo "    $(ls -lh "$UF2" | awk '{print $5, $9}')"

# ── Flash via picotool ───────────────────────────────────────────
# `picotool load -x` requires the device to be in BOOTSEL. If it's
# running our last firmware, we can punt it into BOOTSEL via
# `picotool reboot -u` (uses the picoboot stdio interface that
# pico_stdlib initialises). After load, -x re-enters the new
# firmware, which then boots and presents USB-CDC.
load_cmd=( picotool load -v -x "$UF2" )

echo "==> picotool load (will retry once after a -u reboot if needed)"
if ! "${load_cmd[@]}" 2>&1 | tee /tmp/picotool.last; then
    if grep -qE "No accessible RP-series" /tmp/picotool.last; then
        cat <<EOF >&2

picotool didn't find a board in BOOTSEL mode. Either:
  - hold BOOTSEL while plugging in the Waveshare RP2350-CAN, or
  - have the previous firmware running with USB-CDC enabled (then we
    can punt it into BOOTSEL automatically).

Common gotchas:
  - missing udev rule for picoboot — see the picotool README's
    "Permissions" section, or add yourself to the 'plugdev' group.
  - USB cable that's power-only (no data lines). Try another cable.

Fallback: copy the UF2 to the mass-storage device that appears when
the board boots into BOOTSEL:
  cp $UF2 /run/media/\$USER/RP2350/

EOF
        exit 1
    fi
    # Some other failure — surface it.
    echo "error: picotool load failed" >&2
    exit 1
fi

# ── Wait for new firmware's USB-CDC, attach ──────────────────────
# Filter by Raspberry Pi VID (0x2E8A) so we don't grab a CAN-USB
# adapter or some other ttyACM that happens to be on the host.
pick_pico_serial() {
    for d in /dev/ttyACM*; do
        [ -e "$d" ] || continue
        vid=$(udevadm info -q property "$d" 2>/dev/null \
              | sed -n 's/^ID_VENDOR_ID=//p')
        if [ "$vid" = "2e8a" ]; then
            echo "$d"
            return 0
        fi
    done
    return 1
}

echo "==> Waiting for USB-CDC console (filtering for VID 2e8a)"
SERIAL=""
for _ in $(seq 1 30); do
    SERIAL=$(pick_pico_serial || true)
    if [ -n "$SERIAL" ]; then
        sleep 0.5
        break
    fi
    printf '.'
    sleep 0.5
done
echo

if [ -z "$SERIAL" ]; then
    echo "error: no /dev/ttyACMx appeared within 15s" >&2
    echo "       the firmware may have flashed OK but stdio isn't on USB-CDC" >&2
    exit 1
fi
echo "    found $SERIAL"
echo

# ── Console ──────────────────────────────────────────────────────
echo "==> Attaching $SERIAL @ $BAUD"
if command -v minicom >/dev/null 2>&1; then
    echo "    minicom: Ctrl-A then X to quit"
    exec minicom -D "$SERIAL" -b "$BAUD"
elif command -v screen >/dev/null 2>&1; then
    echo "    screen: Ctrl-A then K to quit"
    exec screen "$SERIAL" "$BAUD"
elif command -v picocom >/dev/null 2>&1; then
    echo "    picocom: Ctrl-A then Ctrl-X to quit"
    exec picocom -b "$BAUD" "$SERIAL"
else
    # Raw cat — no input handling, but works without extra packages.
    stty -F "$SERIAL" "$BAUD" cs8 -cstopb -parenb raw -echo
    exec cat "$SERIAL"
fi

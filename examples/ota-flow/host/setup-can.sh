#!/usr/bin/env bash
# Bring up an slcan-style USB CAN adapter as a SocketCAN interface,
# at the bitrate matching the device-side `CAN_BAUDRATE_KBPS` in
# examples/uds-server/app_config.h (currently 500 kbps).
#
# slcan adapters (CANable v1, generic USB-CAN dongles, etc.) enumerate
# as /dev/ttyACMx character devices; `slcand` bridges that to a
# can-interface that SocketCAN tools (cansend, candump, python-can,
# udsoncan) can talk to. gs_usb-class adapters (CANable Pro) skip the
# slcand step and appear directly as canX — see the gs_usb branch
# below.
#
# Auto-detection: skip any /dev/ttyACMx whose VID is 0x2e8a (Raspberry
# Pi) — that's the RP2350 board's USB-CDC console — and pick the
# remaining one. If 0 or >1 candidates remain, you must specify
# explicitly.
#
# Usage:
#   sudo ./setup-can.sh                  # auto-pick adapter, bring up can0
#   sudo ./setup-can.sh /dev/ttyACM2     # explicit tty, default can0
#   sudo ./setup-can.sh /dev/ttyACM2 can1
#   sudo ./setup-can.sh down             # kill slcand and bring down

set -euo pipefail

CAN_IFACE_DEFAULT="can0"
SLCAN_SPEED="6"          # 6 = 500 kbit/s (matches CAN_BAUDRATE_KBPS=500)

if [ "${EUID:-$(id -u)}" -ne 0 ]; then
    echo "error: run with sudo (need NET_ADMIN to bring CAN up)" >&2
    exit 1
fi

# ── Teardown branch ────────────────────────────────────────────────
if [ "${1:-}" = "down" ]; then
    pkill -f "slcand.*ttyACM" 2>/dev/null || true
    for iface in $(ip -o link show type can 2>/dev/null | awk -F': ' '{print $2}'); do
        ip link set "$iface" down 2>/dev/null || true
        echo "==> $iface down"
    done
    exit 0
fi

# ── Find the slcan adapter's tty ───────────────────────────────────
TTY="${1:-}"
if [ -z "$TTY" ]; then
    candidates=()
    for d in /dev/ttyACM*; do
        [ -e "$d" ] || continue
        vid=$(udevadm info -q property "$d" 2>/dev/null \
              | sed -n 's/^ID_VENDOR_ID=//p')
        # Skip the RP2350 board's USB-CDC console (VID 2e8a = Pi).
        [ "$vid" = "2e8a" ] && continue
        candidates+=("$d")
    done

    if [ ${#candidates[@]} -eq 0 ]; then
        echo "error: no candidate /dev/ttyACM* found (after skipping VID 2e8a)" >&2
        echo "       plug the CAN adapter in, or pass it explicitly." >&2
        exit 1
    fi
    if [ ${#candidates[@]} -gt 1 ]; then
        echo "error: multiple non-Pico ttyACM devices — disambiguate:" >&2
        for c in "${candidates[@]}"; do
            vid=$(udevadm info -q property "$c" 2>/dev/null \
                  | sed -n 's/^ID_VENDOR_ID=//p')
            mdl=$(udevadm info -q property "$c" 2>/dev/null \
                  | sed -n 's/^ID_MODEL=//p')
            echo "  $c  VID=$vid  $mdl" >&2
        done
        exit 1
    fi
    TTY="${candidates[0]}"
fi

CAN_IFACE="${2:-$CAN_IFACE_DEFAULT}"

if [ ! -c "$TTY" ]; then
    echo "error: $TTY is not a character device" >&2
    exit 1
fi

# ── Kill any existing slcand on this tty ──────────────────────────
if pgrep -f "slcand.*$(basename "$TTY")" >/dev/null 2>&1; then
    echo "==> killing existing slcand on $TTY"
    pkill -f "slcand.*$(basename "$TTY")" || true
    sleep 0.5
fi

# ── Take down the iface if it's still around from a previous run ──
if ip link show "$CAN_IFACE" &>/dev/null; then
    ip link set "$CAN_IFACE" down 2>/dev/null || true
fi

# ── Spawn slcand ───────────────────────────────────────────────────
# -o = open the slcan device
# -c = close on exit
# -sN = bitrate (6 = 500 kbit/s)
# Detached daemon; logs to syslog.
echo "==> slcand -o -c -s${SLCAN_SPEED} $TTY $CAN_IFACE"
slcand -o -c -s"$SLCAN_SPEED" "$TTY" "$CAN_IFACE"
sleep 0.5

# ── Bring the interface up ────────────────────────────────────────
ip link set "$CAN_IFACE" up
ip link set "$CAN_IFACE" txqueuelen 1000

# ── Show what we did ──────────────────────────────────────────────
echo
ip -details -statistics link show "$CAN_IFACE"
echo
echo "ready: $CAN_IFACE @ 500 kbps   (bridged from $TTY)"
echo "    next: cd $(dirname "$0") && python3 tester.py $CAN_IFACE"
echo "    teardown: sudo ./setup-can.sh down"

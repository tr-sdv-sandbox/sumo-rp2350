#!/usr/bin/env bash
# Bring up a SocketCAN interface for talking to the sumo-rp2350
# uds-server example.
#
# Targets gs_usb-class adapters (CANable Pro, etc.); other adapters
# (PEAK-USB, slcan-style, Kvaser) need their own kernel-module
# coaxing — adjust below.
#
# Usage:
#   sudo ./setup-can.sh                 # bring up can0 @ 500 kbps
#   sudo ./setup-can.sh can1 250000     # different iface / bitrate
#   sudo ./setup-can.sh can0 down       # tear down
#
# Bitrate must match the device-side `CAN_BAUDRATE_KBPS` in
# examples/uds-server/app_config.h (currently 500 kbps).

set -euo pipefail

IFACE="${1:-can0}"
BITRATE_OR_DOWN="${2:-500000}"

if [ "${EUID:-$(id -u)}" -ne 0 ]; then
    echo "error: run with sudo (need NET_ADMIN to bring CAN up)" >&2
    exit 1
fi

if [ "$BITRATE_OR_DOWN" = "down" ]; then
    if ip link show "$IFACE" &>/dev/null; then
        echo "==> ip link set $IFACE down"
        ip link set "$IFACE" down
    else
        echo "$IFACE not present — nothing to do"
    fi
    exit 0
fi

BITRATE="$BITRATE_OR_DOWN"

# ── 1. Load gs_usb if not present ─────────────────────────────────
if ! lsmod | grep -q '^gs_usb'; then
    echo "==> modprobe gs_usb"
    modprobe gs_usb
    sleep 0.5
fi

# ── 2. Confirm the iface showed up ────────────────────────────────
if ! ip link show "$IFACE" &>/dev/null; then
    echo "error: $IFACE not present after gs_usb load" >&2
    echo "       is the adapter plugged in? available CAN ifaces:" >&2
    ip link show type can 2>/dev/null || echo "         (none)" >&2
    exit 1
fi

# ── 3. Take down if already up ────────────────────────────────────
if ip link show "$IFACE" up &>/dev/null; then
    echo "==> $IFACE already up — bringing down to reconfigure"
    ip link set "$IFACE" down
fi

# ── 4. Configure + bring up ───────────────────────────────────────
echo "==> ip link set $IFACE type can bitrate $BITRATE"
ip link set "$IFACE" type can bitrate "$BITRATE"
ip link set "$IFACE" txqueuelen 1000
ip link set "$IFACE" up

# ── 5. Show what we did ───────────────────────────────────────────
echo
ip -details -statistics link show "$IFACE"
echo
echo "ready: $IFACE @ ${BITRATE} bps"
echo "    next: cd $(dirname "$0") && python3 tester.py $IFACE"

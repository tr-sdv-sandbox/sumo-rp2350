#!/usr/bin/env bash
# Persistent console viewer for the RP2350 board.
#
# Auto-picks the /dev/ttyACMx whose USB VID is 0x2e8a (Raspberry Pi)
# so it ignores the slcan adapter (CANable etc) sharing the same ACM
# pool. Reconnects automatically when the device disappears, e.g.
# after a Flash-Update Boot or watchdog reset during an OTA cycle.
#
# Usage:
#   ./log-console.sh             # tail forever, auto-reconnect
#   ./log-console.sh -t          # timestamp each line
#   ./log-console.sh /dev/ttyACM2  # explicit device, skip auto-pick
#
# Ctrl-C to quit.

set -u

BAUD=115200
EXPLICIT_DEV=""
WITH_TS=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        -t|--timestamp) WITH_TS=1 ;;
        -h|--help)
            sed -n '2,15p' "$0"; exit 0 ;;
        /dev/*) EXPLICIT_DEV="$1" ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
    shift
done

pick_pico() {
    if [ -n "$EXPLICIT_DEV" ] && [ -c "$EXPLICIT_DEV" ]; then
        echo "$EXPLICIT_DEV"
        return 0
    fi
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

stamp_or_pass() {
    if [ "$WITH_TS" = "1" ]; then
        # awk strftime is per-line; works for piped streams.
        awk '{ printf "[%s] %s\n", strftime("%H:%M:%S"), $0; fflush(); }'
    else
        cat
    fi
}

echo "ota-flow console (Ctrl-C to quit)"
echo "================================="

while true; do
    DEV="$(pick_pico || true)"
    if [ -z "$DEV" ]; then
        printf '\r[waiting for /dev/ttyACMx with VID 2e8a …] '
        sleep 0.5
        continue
    fi
    echo
    echo "[+] $DEV @ ${BAUD}"
    stty -F "$DEV" "$BAUD" cs8 -cstopb -parenb raw -echo \
        -echoe -echok -echoctl -echoke 2>/dev/null || true
    cat "$DEV" 2>/dev/null | stamp_or_pass
    # If we get here, the device went away (likely rebooted). Loop.
    echo
    echo "[!] $DEV disappeared — waiting for reconnect"
done

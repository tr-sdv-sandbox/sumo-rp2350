#!/usr/bin/env bash
# Control test: load v1.1.0 (TBYB=ON) directly to App-B with picotool
# in TWO separate picotool invocations, so we can tell whether:
#   1. The load itself works (verify by reading back metadata)
#   2. A separate FUB reboot via picotool's reboot2 cmd works
#
# We've already established that `picotool load -v -p 1 -x -f` produces
# boot_type=N on the device side — meaning the -x's FUB never actually
# made it to a real bootrom FUB-boot. Splitting the steps tells us
# whether the failure is in the load step or in the reboot step.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="/tmp/ota-fub-test-tbyb"
TARGET="sumo_rp2350_ota-flow"
UF2="$BUILD_DIR/${TARGET}.uf2"

ENV_FILE="$SCRIPT_DIR/../../env.sh"
if [ -f "$ENV_FILE" ]; then
    # shellcheck source=/dev/null
    . "$ENV_FILE"
fi

echo "==> Building v1.1.0 TBYB=ON into $BUILD_DIR"
mkdir -p "$BUILD_DIR"
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
      -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release \
      -DOTA_VERSION_MAJOR=1 -DOTA_VERSION_MINOR=1 -DOTA_VERSION_PATCH=0 \
      -DOTA_TBYB=ON > /dev/null
cmake --build "$BUILD_DIR" -j --target "$TARGET" 2>&1 | tail -3

echo
echo "==> Step 1: load to App-B (no execute, just write)"
echo "    picotool load -v -p 1 $UF2 -f"
picotool load -v -p 1 "$UF2" -f
LOAD_RC=$?
echo "    load rc=$LOAD_RC"

echo
echo "==> Step 2: read back partition info to confirm what's in App-B"
picotool info -a -f 2>&1 | grep -E \
    "Partition|version|tbyb|hash:|image type:|binary end" \
    || true

echo
echo "==> Step 3: standalone FUB reboot via picotool (no load this time)"
echo "    picotool reboot -f -a   (just reboot back to app — observe what happens)"
picotool reboot -f -a
REBOOT_RC=$?
echo "    reboot rc=$REBOOT_RC"

echo
echo "==> Done. Watch the log-console terminal:"
echo "    - If header reads v1.1.0: bootrom picked App-B based on version"
echo "      ranking (it should: 1.1 > 1.0). FUB-specific path is a"
echo "      separate concern."
echo "    - If header reads v1.0.0 with boot_type=N: bootrom didn't"
echo "      consider App-B. Likely because TBYB=not bought blocks normal"
echo "      A/B picking until a successful FUB-buy cycle has happened."

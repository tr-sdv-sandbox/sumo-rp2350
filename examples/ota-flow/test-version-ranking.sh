#!/usr/bin/env bash
# Test that the bootrom's normal A/B picker correctly chooses the
# higher-version partition when neither side has TBYB. This sidesteps
# the TBYB/FUB pipeline entirely — we'll rely on:
#
#   - bootrom version ranking for "boot the new image"
#   - app-level trial counter (in littlefs) for the trial/commit window
#   - explicit IMAGE_DEF erase for rollback (forces bootrom to fall
#     back to the surviving slot on next reset)
#
# Steps:
#   1. Build v1.1.0 with TBYB=OFF
#   2. Load it to App-B
#   3. picotool reboot -f -a   (normal reboot back to app)
#   4. Bootrom's A/B picker should choose App-B (version 1.1 > 1.0)
#
# Watch ./host/log-console.sh in another terminal for the verdict.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="/tmp/ota-novarbn-test"
TARGET="sumo_rp2350_ota-flow"
UF2="$BUILD_DIR/${TARGET}.uf2"

ENV_FILE="$SCRIPT_DIR/../../env.sh"
if [ -f "$ENV_FILE" ]; then
    # shellcheck source=/dev/null
    . "$ENV_FILE"
fi

echo "==> Building v1.1.0 TBYB=OFF into $BUILD_DIR"
mkdir -p "$BUILD_DIR"
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
      -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release \
      -DOTA_VERSION_MAJOR=1 -DOTA_VERSION_MINOR=1 -DOTA_VERSION_PATCH=0 \
      -DOTA_TBYB=OFF > /dev/null
cmake --build "$BUILD_DIR" -j --target "$TARGET" 2>&1 | tail -3

echo
echo "==> Step 1: load v1.1.0 (no TBYB) to App-B"
picotool load -v -p 1 "$UF2" -f
echo "    rc=$?"

echo
echo "==> Step 2: confirm App-B metadata"
picotool info -a -f 2>&1 \
    | grep -E "Partition|version|tbyb|hash:|image type:|binary end" \
    || true

echo
echo "==> Step 3: normal reboot back to app — bootrom should pick higher-version slot"
picotool reboot -f -a
echo "    rc=$?"

echo
echo "==> Watch the device console:"
echo "    - v1.1.0 + partition=1 = bootrom auto-picked App-B by version (good)"
echo "    - v1.0.0 + partition=0 = bootrom doesn't auto-rank by version on this config"

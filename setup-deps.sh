#!/usr/bin/env bash
# setup-deps.sh — install the host toolchain + Pico SDK needed to
# build sumo-rp2350 firmware on this machine.
#
# Idempotent: re-runnable. The Pico SDK is installed once per version
# under ~/.local/share/pico-sdk/<version>/ so several Pico projects
# on the same host can share one checkout (each project pins its own
# version via this script). picotool is built against that SDK and
# installed to /usr/local/bin.
#
# After:  source ./env.sh  &&  cd examples/minimal && mkdir build && cd build && cmake .. && make
#
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

# ── Pinned versions ────────────────────────────────────────────────
PICO_SDK_VERSION="2.2.0"
PICOTOOL_VERSION="2.2.0"

# ── Paths ──────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
SDK_DIR="$DATA_HOME/pico-sdk/$PICO_SDK_VERSION"
PICOTOOL_SRC="$DATA_HOME/picotool-src/$PICOTOOL_VERSION"
ENV_FILE="$SCRIPT_DIR/env.sh"

# ── Distro check ───────────────────────────────────────────────────
if ! command -v apt-get >/dev/null 2>&1; then
    cat >&2 <<EOF
error: setup-deps.sh targets Debian/Ubuntu. Install equivalents manually:
       cmake build-essential git python3 libusb-1.0-0-dev
       gcc-arm-none-eabi (includes arm-none-eabi-g++)
       libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib
       gdb-multiarch
       picotool (build from source against the Pico SDK)
EOF
    exit 1
fi

# ── Apt packages ───────────────────────────────────────────────────
echo "==> Installing host packages (sudo)"
sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends \
    cmake \
    build-essential \
    git \
    python3 \
    libusb-1.0-0-dev \
    gcc-arm-none-eabi \
    libnewlib-arm-none-eabi \
    libstdc++-arm-none-eabi-newlib \
    gdb-multiarch

# ── Pico SDK ───────────────────────────────────────────────────────
echo "==> Pico SDK $PICO_SDK_VERSION → $SDK_DIR"
mkdir -p "$(dirname "$SDK_DIR")"
if [ -d "$SDK_DIR/.git" ]; then
    have="$(git -C "$SDK_DIR" describe --tags 2>/dev/null || echo unknown)"
    if [ "$have" = "$PICO_SDK_VERSION" ]; then
        echo "    already at $PICO_SDK_VERSION"
    else
        echo "    found $have; expected $PICO_SDK_VERSION"
        echo "    leaving the existing tree alone — remove $SDK_DIR to re-pin"
    fi
else
    git clone --branch "$PICO_SDK_VERSION" --depth 1 \
        https://github.com/raspberrypi/pico-sdk.git "$SDK_DIR"
    git -C "$SDK_DIR" submodule update --init --recursive --depth 1
fi

# ── picotool (build from source — not packaged on Ubuntu 24.04) ────
need_picotool=1
if command -v picotool >/dev/null 2>&1; then
    have="$(picotool version 2>&1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1 || true)"
    if [ "$have" = "$PICOTOOL_VERSION" ]; then
        echo "==> picotool already at $PICOTOOL_VERSION"
        need_picotool=0
    else
        echo "==> Replacing picotool $have with $PICOTOOL_VERSION"
    fi
fi

if [ "$need_picotool" = 1 ]; then
    echo "==> Building picotool $PICOTOOL_VERSION → /usr/local/bin/picotool"
    mkdir -p "$(dirname "$PICOTOOL_SRC")"
    if [ ! -d "$PICOTOOL_SRC/.git" ]; then
        git clone --branch "$PICOTOOL_VERSION" --depth 1 \
            https://github.com/raspberrypi/picotool.git "$PICOTOOL_SRC"
    fi
    (
        cd "$PICOTOOL_SRC"
        rm -rf build
        mkdir build && cd build
        PICO_SDK_PATH="$SDK_DIR" cmake ..
        make -j"$(nproc)"
        sudo make install
    )
fi

# ── Project-local env helper ───────────────────────────────────────
cat > "$ENV_FILE" <<EOF
# source ./env.sh  before building this project
export PICO_SDK_PATH="$SDK_DIR"
EOF

# ── Done ───────────────────────────────────────────────────────────
echo
echo "==> Setup complete"
echo "    arm-none-eabi-gcc: $(arm-none-eabi-gcc --version | head -1)"
echo "    picotool:          $(picotool version 2>&1 | head -1)"
echo "    Pico SDK:          $SDK_DIR"
echo
echo "Next:"
echo "    source ./env.sh"
echo "    cd examples/minimal && mkdir -p build && cd build && cmake .. && make"

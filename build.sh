#!/usr/bin/env bash
# Configures and builds the firmware for one or more Pico boards, producing
# 6502-picocalc-<board>.uf2 in build/. Defaults to both "pico" and "pico2".
#
# Usage: ./build.sh [board ...]
#   ./build.sh            # build pico and pico2
#   ./build.sh pico2      # build just pico2

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# The gcc-arm-embedded Homebrew cask installs via a GUI pkg installer, which
# can't be authorized headlessly, so the toolchain here is a plain tarball
# extracted to ~/toolchains instead. Override TOOLCHAIN_BIN if yours differs.
TOOLCHAIN_BIN="${TOOLCHAIN_BIN:-$HOME/toolchains/arm-gnu-toolchain-15.2.rel1-darwin-arm64-arm-none-eabi/bin}"
export PATH="$TOOLCHAIN_BIN:$PATH"
export PICO_SDK_PATH="$SCRIPT_DIR/pico-sdk"

BOARDS=("$@")
if [ ${#BOARDS[@]} -eq 0 ]; then
    BOARDS=(pico pico2)
fi

for BOARD in "${BOARDS[@]}"; do
    BUILD_DIR="$SCRIPT_DIR/build/$BOARD"
    echo "==> Configuring for $BOARD"
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -G Ninja -DPICO_BOARD="$BOARD"
    echo "==> Building for $BOARD"
    cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.ncpu)"
    OUT="$SCRIPT_DIR/build/6502-picocalc-$BOARD.uf2"
    cp "$BUILD_DIR/src/6502-picocalc.uf2" "$OUT"
    echo "==> $OUT"
done

#!/usr/bin/env bash
# Builds and runs the cartridge mapper's tests on the host. No Pico, no SDK.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"
mkdir -p build

cc -std=gnu11 -Wall -Wextra -O1 \
   -I shim -I ../../src/machine \
   test_cart.c ../../src/machine/cart.c \
   -o build/test_cart

./build/test_cart

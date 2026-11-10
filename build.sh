#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# Build the firmware: ./build.sh [make args...]
#
#   ./build.sh BUILD=build-net                    network + sound + SSH (default NET=1)
#   ./build.sh LVGL=1 BUILD=build-lvgl-net        the same plus LVGL
#   ./build.sh NET=0 BUILD=build-plain            plain interpreter, BRAM only
#
# Result: ports/vixen/<BUILD>/firmware.rvk (plus .elf/.bin/.map).
#
# Toolchain: any rv32 GCC with a multilib for rv32i(m) WITHOUT the C extension
# (the VIXEN core has none). The releases are built with Espressif's
# riscv32-esp-elf GCC 14.2 (esp-14.2.0_20241119), compiling for rv32im_zicsr
# and linking against its rv32i_zicsr_zifencei multilib. Point VIXEN_TOOLCHAIN
# at its bin/ directory if it is not on PATH:
#
#   VIXEN_TOOLCHAIN=~/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20241119/riscv32-esp-elf/bin
#
# VIXEN_DEVKIT overrides the devkit location (default deps/vixen32-devkit).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
: "${VIXEN_TOOLCHAIN:=$HOME/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20241119/riscv32-esp-elf/bin}"
[ -d "$VIXEN_TOOLCHAIN" ] && export PATH="$VIXEN_TOOLCHAIN:$PATH"
T=${CROSS_COMPILE:-riscv32-esp-elf-}
command -v "${T}gcc" >/dev/null || { echo "${T}gcc not found: set VIXEN_TOOLCHAIN"; exit 1; }

DEVKIT=$(cd "${VIXEN_DEVKIT:-$ROOT/deps/vixen32-devkit}" 2>/dev/null && pwd) \
    || { echo "VIXEN-32 devkit not found: run VIXEN_DEVKIT=/path/to/vixen32-devkit ./setup.sh"; exit 1; }
# The devkit ships mkrvpk as C source; build the host tool once.
[ -x "$DEVKIT/tools/mkrvpk" ] || cc -O2 -o "$DEVKIT/tools/mkrvpk" "$DEVKIT/tools/mkrvpk.c"

# Compile with -march=rv32im_zicsr (Makefile), link with the no-C multilib.
LDC="${T}gcc -march=rv32i_zicsr_zifencei -mabi=ilp32"
make -C "$ROOT/ports/vixen" -j"$(nproc)" CROSS_COMPILE="$T" DEVKIT_DIR="$DEVKIT" \
    LD="$LDC" LIBS="$($LDC -print-libgcc-file-name) -lm" "$@"

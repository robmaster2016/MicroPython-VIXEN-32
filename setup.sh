#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# Fetch everything the VIXEN-32 port builds against into deps/:
#
#   deps/micropython              MicroPython v1.29.0 (+ lwip, mbedtls, micropython-lib)
#   deps/lv_binding_micropython   LVGL binding (+ LVGL 9.3), patched for the VIXEN
#   deps/usermods/lvgl            -> ../lv_binding_micropython (USER_C_MODULES)
#   deps/micropython/mpy-cross    built once (needed to freeze .py modules)
#   deps/vixen32-devkit           symlink to your VIXEN-32 devkit (optional:
#                                 VIXEN_DEVKIT=/path/to/vixen32-devkit ./setup.sh)
#
# Safe to run again: existing checkouts are left alone.
set -euo pipefail

MPY_TAG=v1.29.0
LVB_COMMIT=032dda4b7e8419702a4b9c4c8748a4d85d51be61   # lv_binding_micropython master, LVGL 9.3.0

ROOT=$(cd "$(dirname "$0")" && pwd)
DEPS=$ROOT/deps
mkdir -p "$DEPS/usermods"

if [ ! -d "$DEPS/micropython/.git" ]; then
    echo "== MicroPython $MPY_TAG"
    git clone --depth 1 --branch "$MPY_TAG" https://github.com/micropython/micropython.git "$DEPS/micropython"
    git -C "$DEPS/micropython" submodule update --init --depth 1 lib/lwip lib/mbedtls lib/micropython-lib
fi

if [ ! -d "$DEPS/lv_binding_micropython/.git" ]; then
    echo "== lv_binding_micropython ${LVB_COMMIT:0:7}"
    git clone https://github.com/lvgl/lv_binding_micropython.git "$DEPS/lv_binding_micropython"
    git -C "$DEPS/lv_binding_micropython" checkout -q "$LVB_COMMIT"
    git -C "$DEPS/lv_binding_micropython" submodule update --init lvgl pycparser
    echo "== applying patches/lv_binding-vixen.patch"
    (cd "$DEPS/lv_binding_micropython" && patch -p1 --forward < "$ROOT/patches/lv_binding-vixen.patch")
fi
ln -sfn ../lv_binding_micropython "$DEPS/usermods/lvgl"

if [ ! -x "$DEPS/micropython/mpy-cross/build/mpy-cross" ]; then
    echo "== mpy-cross"
    make -C "$DEPS/micropython/mpy-cross" -j"$(nproc)"
fi

if [ -n "${VIXEN_DEVKIT:-}" ]; then
    ln -sfn "$(cd "$VIXEN_DEVKIT" && pwd)" "$DEPS/vixen32-devkit"
fi
if [ ! -f "$DEPS/vixen32-devkit/common/vih_regs.h" ]; then
    echo
    echo "Note: no VIXEN-32 devkit linked yet. It ships with the VIXEN-32 core"
    echo "release (vixen32-devkit.zip). Run: VIXEN_DEVKIT=/path/to/vixen32-devkit ./setup.sh"
fi
echo "Done. Next: ./build.sh BUILD=build-net   (see docs/building.md)"

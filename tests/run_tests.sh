#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# Run the test suite in the emulator (ports/vixen/tools/emu/vxemu.py).
#
#   tests/run_tests.sh                 all tests against build-net
#   tests/run_tests.sh test_crypto.py  selected tests
#   FW=ports/vixen/build-lvgl-net/firmware.elf tests/run_tests.sh
#   OFFLINE=1 tests/run_tests.sh       skip tests that need the internet
#
# A test passes when the script prints "TEST OK" and raises nothing.
# test_*.sh files are host-side tests (they start the emulator themselves).
# Needs: a firmware build (./build.sh BUILD=build-net), python3, and
# qemu-system-x86_64 (user networking for the emulated Ethernet).
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
EMU=$ROOT/ports/vixen/tools/emu
FW=$(realpath -m "${FW:-$ROOT/ports/vixen/build-net/firmware.elf}")
OUT=$ROOT/tests/out
NET_TESTS="test_net.py test_http.py test_https_mip.py test_examples.py"
mkdir -p "$OUT"

[ -f "$FW" ] || { echo "no firmware at $FW -- build it first (./build.sh BUILD=build-net)"; exit 2; }
if [ ! -x "$EMU/venv/bin/python" ]; then
    echo "== creating emulator venv (unicorn)"
    python3 -m venv "$EMU/venv" && "$EMU/venv/bin/pip" -q install unicorn || exit 2
fi
export FW EMU OUT

cd "$ROOT/tests"
if [ $# -gt 0 ]; then TESTS="$*"; else TESTS=$(ls test_*.py test_*.sh); fi

pass=0; fail=0; skip=0; failed=""
for t in $TESTS; do
    if [ -n "${OFFLINE:-}" ] && [[ " $NET_TESTS test_sshd.sh " == *" $t "* ]]; then
        printf '%-22s SKIP (offline)\n' "$t"; skip=$((skip + 1)); continue
    fi
    log=$OUT/${t%.*}.log
    start=$(date +%s)
    if [[ $t == *.sh ]]; then
        bash "$t" > "$log" 2>&1
    elif grep -q "^# vx: examples" "$t"; then
        # a RAM /sd holding ports/vixen/examples, on sys.path as on the card
        { cat "$EMU/sd_ramfs.py"
          echo "import binascii, os, sys"
          echo "A = '/sd/vixen-32/apps/micropython'"
          echo "for d in ('vixen-32/download', 'vixen-32/apps/micropython/examples'): sd.dirs.add(d)"
          for f in "$ROOT"/ports/vixen/examples/*.py "$ROOT"/ports/vixen/examples/*.sid; do
              printf "with open(A + '/examples/%s', 'wb') as f: f.write(binascii.a2b_base64('%s'))\n" \
                  "$(basename "$f")" "$(base64 -w0 "$f")"
          done
          echo "sys.path.append(A + '/examples')"
          cat "$t"; } > "$OUT/${t%.*}.run.py"
        "$EMU/venv/bin/python" "$EMU/vxemu.py" "$FW" "$OUT/${t%.*}.run.py" 300 > "$log" 2>&1
    else
        "$EMU/venv/bin/python" "$EMU/vxemu.py" "$FW" "$t" 300 > "$log" 2>&1
    fi
    secs=$(( $(date +%s) - start ))
    if grep -q "TEST OK" "$log" && ! grep -q "Traceback\|TEST FAILED\|TIMEOUT/STOP" "$log"; then
        printf '%-22s PASS  (%ss)\n' "$t" "$secs"; pass=$((pass + 1))
    else
        printf '%-22s FAIL  (%ss, see %s)\n' "$t" "$secs" "${log#$ROOT/}"; fail=$((fail + 1)); failed="$failed $t"
    fi
done
echo "passed $pass, failed $fail, skipped $skip"
[ $fail -eq 0 ] || { echo "failed:$failed"; exit 1; }

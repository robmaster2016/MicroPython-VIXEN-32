#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# sshd end to end with the host's OpenSSH: public-key login, upload with
# legacy scp (-O) and download with sftp (scp's default), compare the files;
# then the REPL over ssh with paste mode (Ctrl-E ... Ctrl-D).
# The emulated board gets a RAM /sd (tools/emu/sd_ramfs.py); host port 2222
# is forwarded to its port 22. Called by run_tests.sh (needs FW, EMU, OUT).
set -u
W=$(mktemp -d "$OUT/sshd.XXXXXX")
trap 'kill $EMU_PID 2>/dev/null; wait 2>/dev/null; rm -rf "$W"' EXIT
PORT=${SSHD_TEST_PORT:-2222}

ssh-keygen -q -t ed25519 -N "" -C test@host -f "$W/id"
head -c 150000 /dev/urandom > "$W/up.bin"

cat "$EMU/sd_ramfs.py" > "$W/board.py"
cat >> "$W/board.py" <<EOF
import network, time, sshd
lan = network.LAN(); lan.active(True)
# static address: QEMU forwards the host port to 10.0.2.15
lan.ifconfig(("10.0.2.15", "255.255.255.0", "10.0.2.2", "10.0.2.3"))
sshd._mkdirs(sshd.SSH_DIR)
with open(sshd.AUTH_KEYS, "w") as f:
    f.write("$(cat "$W/id.pub")\n")
sshd.start()
print("SSHD READY")
EOF

VX_STAY=1 VX_FWD="tcp:127.0.0.1:$PORT-:22" "$EMU/venv/bin/python" "$EMU/vxemu.py" "$FW" "$W/board.py" 240 > "$W/emu.log" 2>&1 &
EMU_PID=$!
for _ in $(seq 120); do grep -q "SSHD READY" "$W/emu.log" && break; sleep 0.5; done
grep -q "SSHD READY" "$W/emu.log" || { cat "$W/emu.log"; echo "sshd did not start"; exit 1; }

OPTS=(-i "$W/id" -o IdentitiesOnly=yes -o BatchMode=yes -o StrictHostKeyChecking=no
      -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=30)
ok=1
scp -O "${OPTS[@]}" -P "$PORT" "$W/up.bin" vixen@127.0.0.1:test.bin || ok=0
scp "${OPTS[@]}" -P "$PORT" vixen@127.0.0.1:test.bin "$W/down.bin" || ok=0
cmp -s "$W/up.bin" "$W/down.bin" && echo "scp upload + sftp download: 150000 bytes identical" || ok=0

# Ctrl-E, two lines of code, Ctrl-D runs them; the final Ctrl-D ends the session
(sleep 3; printf '\x05'; sleep 1
 printf 'def magic(n):\r    return sum(range(n))\r\rprint("paste", magic(101))\r'
 sleep 1; printf '\x04'; sleep 3; printf '\x04'; sleep 1) |
    timeout 60 ssh -tt "${OPTS[@]}" -p "$PORT" vixen@127.0.0.1 > "$W/repl.txt" 2>&1
tr -d '\r' < "$W/repl.txt"
grep -q "^paste 5050" <(tr -d '\r' < "$W/repl.txt") && echo "paste mode over ssh: ok" || ok=0

if [ $ok = 1 ]; then
    echo "TEST OK"
else
    echo "--- emulator log"; tail -n 40 "$W/emu.log"
    echo "TEST FAILED"
fi

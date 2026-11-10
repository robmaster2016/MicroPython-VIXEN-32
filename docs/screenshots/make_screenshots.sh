#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# Re-create the screenshots in this folder with the emulator (VX_SCREEN):
# real firmware, real network, native VIXEN-32 resolution 384x224.
#
#   VX_FONT=<vixen32 core source>/rtl/font_rom.hex docs/screenshots/make_screenshots.sh
#
# The ROM font (text screens) is the MEGA65 ASCII font by deft; it ships with
# the VIXEN-32 core source. Needs ports/vixen/build-lvgl-net (./build.sh).
# Weather and clock fetch live data, so every run looks a little different.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/docs/screenshots
EMU=$ROOT/ports/vixen/tools/emu
EX=$ROOT/ports/vixen/examples
FW=${FW:-$ROOT/ports/vixen/build-lvgl-net/firmware.elf}
: "${VX_FONT:?set VX_FONT to rtl/font_rom.hex of the VIXEN-32 core source}"
export VX_FONT VX_SCREEN=1
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
PY=$EMU/venv/bin/python

# Board set-up: a RAM /sd with every example in examples/, as on the card.
{
    cat "$EMU/sd_ramfs.py"
    echo "import binascii, os, sys"
    echo "A = '/sd/vixen-32/apps/micropython'"
    echo "for d in ('examples', 'lib', '../../download'): sd.dirs.add(('vixen-32/apps/micropython/' + d).replace('apps/micropython/../../', ''))"
    # the WAV cut to 8 s: the RAM /sd lives in the Python heap
    "$PY" - "$EX/aurec-jazz.wav" "$W/aurec-jazz.wav" <<'PYEOF'
import sys, struct
d = open(sys.argv[1], "rb").read()
i = d.index(b"data")
rate, align = struct.unpack("<I", d[24:28])[0], struct.unpack("<H", d[32:34])[0]
n = min(struct.unpack("<I", d[i + 4:i + 8])[0], rate * align * 8)
body = d[12:i] + b"data" + struct.pack("<I", n) + d[i + 8:i + 8 + n]
open(sys.argv[2], "wb").write(b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body)
PYEOF
    for f in "$EX"/*.py "$EX"/*.png "$EX"/*.sid "$W/aurec-jazz.wav"; do
        printf "with open(A + '/examples/%s', 'wb') as f: f.write(binascii.a2b_base64('%s'))\n" \
            "$(basename "$f")" "$(base64 -w0 "$f")"
    done
    echo "sys.path.append(A + '/lib'); sys.path.append(A + '/examples'); os.chdir(A)"
    # _cls(): clear the console and show the power-on banner again, so a
    # typed session starts on a clean screen (the set-up ran before it)
    echo "def _cls(): print('\x1b[2J\x1b[H' + sys.version.split('; ', 1)[1] + '; ' + sys.implementation._machine + '\nType \"help()\" for more information.')"
} > "$W/setup.py"

# ONLY="clock weather" re-makes just these
want() { [ -z "${ONLY:-}" ] || [[ " $ONLY " == *" $1 "* ]]; }

shot() {   # shot <name> <seconds to settle>  (script on stdin, LVGL screens)
    want "$1" || { cat > /dev/null; return; }
    { cat "$W/setup.py"; cat; printf '\nimport time, machine\ntime.sleep_ms(%d)\nmachine.mem32[0x3A1FFFF0] = 0\n' "$2"; } > "$W/$1.py"
    VX_SHOT="$OUT/$1.png" "$PY" "$EMU/vxemu.py" "$FW" "$W/$1.py" 300 | grep -E "screenshot|Traceback|Error" || true
}

typed() {  # typed <name>  (lines typed into the REPL on stdin, text screens)
    want "$1" || { cat > /dev/null; return; }
    cat > "$W/$1.py"
    VX_TYPE=1 VX_SETUP="$W/setup.py" VX_SHOT_END="$OUT/$1.png" "$PY" "$EMU/vxemu.py" "$FW" "$W/$1.py" 300 \
        | grep -E "screenshot|Traceback|Error" || true
}

typed repl <<'EOF'
_cls()
print("Feel the magic of MicroPython!")
[n * n for n in range(12)]
import sysinfo
EOF

typed network <<'EOF'
_cls()
import net
lan = net.up()
import mip
mip.install("base64")
import wget
wget.get("https://micropython.org/static/img/Mlogo_138wh.png")
EOF

shot weather 3000 <<'EOF'
import weather
weather.show()
EOF

shot clock 2500 <<'EOF'
import clock
clock.start()
EOF

shot lvgl_demo 1500 <<'EOF'
import lvgl_demo
EOF

shot png_demo 1500 <<'EOF'
import png_demo
EOF

shot widgets_gallery 2500 <<'EOF'
import widgets_gallery
EOF

shot sound_wav 0 <<'EOF'
import sys; sys.modules["sound_demo_quiet"] = True
import sound_demo as D, lvgl as lv, sound, time, machine
st = D._Stage(lv)
info = D._wav_info(D.HERE + "/aurec-jazz.wav")
st.labels("aurec-jazz.wav", "%d Hz stereo, %d bit" % (info[1], info[5]))
pic = D._Rosette(D._Gfx(lv, st.scr), info)
sound.wav.play(D.HERE + "/aurec-jazz.wav")
t0 = time.ticks_ms()
while time.ticks_diff(time.ticks_ms(), t0) < 4000:
    pic.frame(time.ticks_diff(time.ticks_ms(), t0))
    time.sleep_ms(5)
machine.mem32[0x3A1FFFF0] = 0      # the picture as it is drawn by the blitter
sound.sid.stop(); sound.wav.stop()
EOF

shot sound_sid 0 <<'EOF'
import sys; sys.modules["sound_demo_quiet"] = True
import sound_demo as D, lvgl as lv, sound, time, machine
st = D._Stage(lv)
t = D._sid_info(D.HERE + "/neon_grid.sid")
st.labels(t[0], "%s  -  %s" % (t[1], t[2]))
pic = D._Harmonograph(D._Gfx(lv, st.scr), t[3])
sound.sid.play(D.HERE + "/neon_grid.sid")
t0 = time.ticks_ms()
while time.ticks_diff(time.ticks_ms(), t0) < 10000:
    pic.frame(time.ticks_diff(time.ticks_ms(), t0))
    time.sleep_ms(5)
machine.mem32[0x3A1FFFF0] = 0      # the picture as it is drawn by the blitter
sound.sid.stop(); sound.wav.stop()
EOF

# 2x copies (nearest neighbour) for the README: crisp pixels on big screens
"$PY" - "$OUT" <<'EOF'
import sys, os, struct, zlib
d = sys.argv[1]
for n in sorted(os.listdir(d)):
    if not n.endswith(".png") or n.endswith("@2x.png"):
        continue
    data = open(os.path.join(d, n), "rb").read()
    w, h = struct.unpack(">II", data[16:24])
    idat = b"".join(data[i + 8:i + 8 + struct.unpack(">I", data[i:i + 4])[0]]
                    for i in range(8, len(data)) if data[i + 4:i + 8] == b"IDAT")
    raw = zlib.decompress(idat)
    rows = [raw[y * (w * 3 + 1) + 1:(y + 1) * (w * 3 + 1)] for y in range(h)]
    big = []
    for r in rows:
        line = b"".join(r[x * 3:x * 3 + 3] * 2 for x in range(w))
        big += [b"\0" + line] * 2
    def chunk(t, c):
        return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c))
    with open(os.path.join(d, n[:-4] + "@2x.png"), "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w * 2, h * 2, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(b"".join(big), 9)) + chunk(b"IEND", b""))
    print("2x", n)
EOF

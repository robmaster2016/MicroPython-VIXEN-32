# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# Capture gfx_demo frames on the real MEGA65 (LVGL build, core v0.93+): the
# emulator does not model the blitter's triangles (texture, perspective,
# Gouraud, Z buffer), so make_screenshots.sh replays a frame the hardware
# drew. Run on the machine (e.g. pasted over SSH); it writes the shown
# framebuffer (512 x 256 bytes, palette indices) for a few fixed moments of
# the animation to gfxcap_<10*t>.bin. On the PC:
#   python3 -c "import zlib,sys; open(sys.argv[2],'wb').write(zlib.compress(open(sys.argv[1],'rb').read(),9))" \
#       gfxcap_9.bin docs/screenshots/gfx_demo.fb.z
import gfx, machine, uctypes, os
A = "/sd/vixen-32/apps/micropython"
g = {}
exec(open(A + "/examples/gfx_demo.py").read().replace("\nmain()\n", "\n"), g)
gfx.zbuffer(True)
tex, tile = g["checker"](), g["grid"]()
sky = gfx.rgb(0, 0, 40)
W, H = gfx.size()
names = []
try:
    for T in (0.9, 1.7, 2.6):
        gfx.open()                       # back buffer = FB1 after open
        gfx.clear(sky)
        g["floor"](T, tile)
        g["cube"](T, tex)
        g["disc"](T, W * 0.15, H * 0.22, H * 0.17)
        gfx.flip()                       # waits for the blitter; FB1 is shown
        machine.mem32[0x3A0001E4] = 1    # forget cached chip RAM (blitter wrote it)
        fb = uctypes.bytearray_at(0x40320000, 512 * 256)
        n = A + "/gfxcap_%d.bin" % int(T * 10)
        f = open(n, "wb")
        for o in range(0, 512 * 256, 4096):
            f.write(fb[o:o + 4096])
        f.close()
        names.append(n)
finally:
    gfx.close()
    tex.free()
    tile.free()
print("CAP", W, H, names)

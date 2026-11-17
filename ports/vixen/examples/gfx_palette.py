# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# gfx_palette.py -- colour cycling, the demo-scene way (LVGL build, core v0.95+).
#
#   import gfx_palette
#
# A plasma is drawn ONCE, in palette indices 0-255. From then on nothing is
# drawn: every frame rotates the 256 colours of palette bank 1, and the
# plasma flows. Bank 0 keeps the standard palette for gfx.rgb(). RUN/STOP
# ends it.

import math
import time

import gfx

W, H = gfx.size()
BLOCK = 4


def gradient(i):
    """Colour of index i: a loop through blue, magenta, orange, yellow."""
    a = i / 256 * 2 * math.pi
    return (int(127 + 127 * math.sin(a)),
            int(127 + 127 * math.sin(a + 2.1)) // 2,
            int(127 + 127 * math.sin(a + 4.2)))


def plasma():
    """Index per 4 x 4 block, from three overlapping waves."""
    for y in range(0, H, BLOCK):
        for x in range(0, W, BLOCK):
            v = (math.sin(x / 23) + math.sin(y / 17) + math.sin((x + y) / 29)
                 + math.sin(math.sqrt((x - W / 2) ** 2 + (y - H / 2) ** 2) / 13))
            gfx.rect(x, y, BLOCK, BLOCK, int((v + 4) * 32) & 255)


def main(seconds=0):
    cols = [gradient(i) for i in range(256)]
    gfx.open()
    for i, (r, g, b) in enumerate(cols):
        gfx.palette(i, r, g, b, bank=1)
    gfx.palette_show(1)
    for _ in range(2):                  # both buffers
        plasma()
        gfx.flip()
    t0 = time.ticks_ms()
    k = frames = 0
    try:
        while not seconds or time.ticks_diff(time.ticks_ms(), t0) < seconds * 1000:
            k = (k + 3) & 255
            for i in range(256):        # rotate the colours, not the pixels
                r, g, b = cols[(i + k) & 255]
                gfx.palette(i, r, g, b, bank=1)
            time.sleep_ms(20)
            frames += 1
    finally:
        dt = time.ticks_diff(time.ticks_ms(), t0) / 1000
        gfx.close()
        print("%d palette rotations, %.1f per second" % (frames, frames / dt if dt else 0))


main()

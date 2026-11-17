# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# gfx_demo.py -- the blitter's triangles from Python (LVGL build, core v0.93+).
#
#   import gfx_demo
#
# A spinning cube over a moving floor, one camera: every face and every
# floor tile is two triangles, each ONE blitter op, textured with perspective
# and drawn through the Z buffer, so nothing needs sorting. Top left a
# Gouraud disc over the grey ramp. RUN/STOP ends it.

import math
import time

import gfx

W, H = gfx.size()


def checker():
    """64 x 64 texture: a checkerboard in two colours with a frame."""
    a, b, edge = gfx.rgb(255, 200, 0), gfx.rgb(200, 40, 40), gfx.rgb(255, 255, 255)
    t = bytearray(64 * 64)
    for y in range(64):
        for x in range(64):
            if x < 2 or y < 2 or x > 61 or y > 61:
                c = edge
            else:
                c = a if (x // 8 + y // 8) & 1 else b
            t[y * 64 + x] = c
    return gfx.Texture(t, 64, 64)


def grid():
    """256 x 256 floor texture: tiles of 64 with a cyan line on two edges.
    The blitter repeats a texture every 256 texels, so a floor that runs on
    needs the full 256 x 256."""
    bg, ln = bytes([gfx.rgb(0, 0, 90)]), bytes([gfx.rgb(0, 220, 255)])
    line = ln * 256
    row = (ln * 3 + bg * 61) * 4
    return gfx.Texture((line * 3 + row * 61) * 4, 256, 256)


CUBE = [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
        (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]
FACES = [(0, 1, 2, 3), (5, 4, 7, 6), (4, 0, 3, 7), (1, 5, 6, 2), (4, 5, 1, 0), (3, 2, 6, 7)]
CX, CY, F = W * 0.5, H * 0.4, H * 0.9      # camera: centre and focal length


def project(x, y, z):
    return (CX + x * F / z, CY + y * F / z, z)


def cube(t, tex):
    ax, ay = t * 0.9, t * 0.6
    sx, cxa, sy, cya = math.sin(ax), math.cos(ax), math.sin(ay), math.cos(ay)
    pts = []
    for x, y, z in CUBE:
        y, z = y * cxa - z * sx, y * sx + z * cxa          # about x
        x, z = x * cya + z * sy, -x * sy + z * cya         # about y
        pts.append(project(x + 0.8, y - 0.2, z + 5.5))
    for a, b, c, d in FACES:
        pa, pb, pc, pd = pts[a], pts[b], pts[c], pts[d]
        # back faces away: the blitter draws both windings
        if (pb[0] - pa[0]) * (pc[1] - pa[1]) - (pb[1] - pa[1]) * (pc[0] - pa[0]) <= 0:
            continue
        gfx.tri_tex(pa + (0, 0), pb + (63, 0), pc + (63, 63), tex, persp=True)
        gfx.tri_tex(pa + (0, 0), pc + (63, 63), pd + (0, 63), tex, persp=True)


def floor(t, tex):
    """A plane at y = 2.2 from z = 2 to 26, 64 texels per unit of 2, the
    texture sliding towards the camera. Quads of 4 x 4 units, 2 triangles
    each; texture coordinates run past 256 and the blitter repeats it."""
    y, scroll = 2.2, (t * 96) % 256
    for z0 in range(2, 26, 4):
        z1 = z0 + 4
        va, vb = z0 * 32 - scroll, z1 * 32 - scroll
        for x0 in range(-6, 6, 4):
            pa, pb = project(x0, y, z0), project(x0 + 4, y, z0)
            pc, pd = project(x0 + 4, y, z1), project(x0, y, z1)
            ua, ub = x0 * 32 + 256, x0 * 32 + 384
            gfx.tri_tex(pa + (ua, va), pb + (ub, va), pc + (ub, vb), tex, persp=True)
            gfx.tri_tex(pa + (ua, va), pc + (ub, vb), pd + (ua, vb), tex, persp=True)


def disc(t, cx, cy, r):
    """Gouraud: bright centre, dark rim, slowly turning and breathing."""
    n, r = 16, r * (0.85 + 0.15 * math.sin(t * 2))
    for i in range(n):
        a0 = t * 0.3 + i * 2 * math.pi / n
        a1 = a0 + 2 * math.pi / n
        gfx.tri_shade((cx, cy, 1, 255),
                      (cx + r * math.cos(a0), cy + r * math.sin(a0), 1, 220),
                      (cx + r * math.cos(a1), cy + r * math.sin(a1), 1, 220))


def main(seconds=0):
    gfx.open()
    gfx.zbuffer(True)
    tex, tile = checker(), grid()
    sky = gfx.rgb(0, 0, 40)
    t0 = time.ticks_ms()
    frames = 0
    try:
        while not seconds or time.ticks_diff(time.ticks_ms(), t0) < seconds * 1000:
            t = time.ticks_diff(time.ticks_ms(), t0) / 1000
            gfx.clear(sky)
            floor(t, tile)
            cube(t, tex)
            disc(t, W * 0.15, H * 0.22, H * 0.17)
            gfx.flip()
            frames += 1
    finally:
        dt = time.ticks_diff(time.ticks_ms(), t0) / 1000
        gfx.close()
        tex.free()
        tile.free()
        print("%d frames, %.1f per second" % (frames, frames / dt if dt else 0))


main()

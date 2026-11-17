# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# vx: screen
# gfx module (LVGL build): the API, clipping, textures and their memory.
# The emulator models FILL, COPY and LINE but not the triangle ops, so the
# triangles here only have to run; fills, blits and lines are checked in the
# frame (chip RAM). In a build without gfx this test passes trivially.
try:
    import gfx
except ImportError:
    gfx = None
    print("no gfx in this build")

if gfx:
    import machine, vixen

    W, H = gfx.size()
    assert (W, H) == vixen.screen_size(), (W, H)
    try:
        gfx.clear()
        raise AssertionError("drew before open()")
    except RuntimeError:
        pass

    gfx.open()
    top = vixen.console_margin()[2]
    STRIDE = 512

    def back_pixel(x, y):
        # both buffers (GFX_PTR is write-only); the test finds the one drawn
        return [machine.mem8[0x40000000 + fb + (top + y) * STRIDE + x] for fb in (0x300000, 0x320000)]

    red = gfx.rgb(255, 0, 0)
    assert 0 <= red < 256 and red != gfx.rgb(0, 0, 255)
    gfx.clear(7)
    gfx.rect(-10, -10, 20, 20, red)            # clipped to 0..9
    gfx.line(-50, 5, W + 50, 5, 9)             # clipped to the screen
    gfx.rect(W - 3, H - 3, 10, 10, 11)
    gfx.flip()
    seen = [back_pixel(x, y) for x, y in ((0, 0), (9, 9), (10, 10), (W - 1, 5), (W - 1, H - 1))]
    fb = 0 if seen[0][0] == red else 1        # whichever buffer was drawn
    got = [p[fb] for p in seen]
    assert got == [red, red, 7, 9, 11], got

    # textures: blit with clipping and a key, memory, free
    t = gfx.Texture(bytes([1, 2, 3, 0] * 4), 4, 4)
    assert (t.width, t.height) == (4, 4)
    gfx.clear(5)
    gfx.blit(t, -1, 0, 0)                      # column 0 cut, key 0 keeps the background
    gfx.flip()
    row = [back_pixel(x, 0)[1 - fb] for x in range(4)]
    assert row == [2, 3, 5, 5], row
    # colour base and LUT on blits: key on the raw byte, then + base, then the table
    gfx.color_base(16)
    gfx.clear(5)
    gfx.blit(t, 0, 0, 0)
    gfx.flip()
    row = [back_pixel(x, 0)[fb] for x in range(4)]
    assert row == [17, 18, 19, 5], row
    gfx.lut(bytes((255 - i) for i in range(256)))
    gfx.clear(5)
    gfx.blit(t, 0, 0, 0)
    gfx.flip()
    row = [back_pixel(x, 0)[1 - fb] for x in range(4)]
    assert row == [238, 237, 236, 5], row
    gfx.lut(None)
    gfx.color_base(0)
    try:
        gfx.lut(bytes(10))
        raise AssertionError("short LUT accepted")
    except ValueError:
        pass

    # palettes: 8 banks
    gfx.palette(1, 255, 0, 0)
    gfx.palette(7, 0, 255, 0, bank=3)
    gfx.palette_cube(5)
    gfx.palette_show(5)
    gfx.palette_show(0)
    for bad in (lambda: gfx.palette_show(8), lambda: gfx.palette(256, 0, 0, 0), lambda: gfx.palette(0, 0, 0, 0, bank=-1)):
        try:
            bad()
            raise AssertionError("bad palette call accepted")
        except ValueError:
            pass

    big = gfx.Texture(bytes(256 * 256), 256, 256)
    gfx.Texture(bytes(256 * 200), 256, 200)    # not kept: gc frees it when needed
    big.free()
    again = gfx.Texture(bytes(256 * 256), 256, 256)
    try:
        gfx.Texture(bytes(4), 0, 4)
        raise AssertionError("w = 0 accepted")
    except ValueError:
        pass

    # triangles: every kind runs (not drawn by the emulator)
    gfx.zbuffer(True)
    gfx.clear(0)
    gfx.tri((10, 10), (100, 20), (50, 90), red)
    gfx.tri((10, 10, 2), (100, 20, 3), (50, 90, 4), red)
    gfx.tri_shade((10, 10, 1, 216), (100, 20, 1, 255), (50, 90, 1, 230))
    gfx.tri_tex((10, 10, 2, 0, 0), (100, 20, 2, 255, 0), (50, 90, 2, 0, 255), again, persp=True)
    gfx.tri_tex((10, 10, 2, 0, 0), (100, 20, 2, 255, 0), (50, 90, 2, 0, 255), again, key=True)
    gfx.tri((-500, -500), (900, 10), (10, 900), 3)     # far off screen: clipped
    gfx.flip()
    gfx.close()
    t.free(); again.free()
    print("gfx ok")
print("TEST OK")

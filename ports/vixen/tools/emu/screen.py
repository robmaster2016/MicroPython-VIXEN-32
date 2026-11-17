# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
"""Display model for vxemu: VIH registers, palette, tile layer, GFX layer and
the blitter's FILL/COPY/LINE, rendered to a PNG at the VIXEN-32's native 384x224.
In hi-res text mode (VIDEO_MODE, core v0.95) the PNG is 768x224: the tile
layer per dot (half a pixel) from its 128-cell map, everything else doubled.
The hi-res layer sits 24 dots further left than the devkit describes
(HIRES_DX, measured on core v0.98; uart.c's CON_HIRES_DX).

Used by vxemu.py when VX_SCREEN is set. Not modelled: sprites (the LVGL mouse
cursor), 8x16 and uploaded fonts,
8bpp glyphs, copper. The 8x8 ROM font is the MEGA65 ASCII font by deft; it
ships with the VIXEN-32 core source (rtl/font_rom.hex) and is read from
there (VX_FONT), not copied into this repository.
"""
import struct
import zlib

W, H = 384, 224
CHIP = 0x40000000

# Palette entries 0-31 after power-on (rtl/palette_ram.sv): C64 colours,
# then a 16-step grey ramp. 32-255 start black.
DEFAULT_PALETTE = [
    0x0000, 0xFFFF, 0x8986, 0x6597, 0x89F2, 0x5509, 0x4191, 0xBE6E,
    0x8AA5, 0x5200, 0xBB4C, 0x528A, 0x7BCF, 0x9711, 0x7B58, 0x9CF3,
    0x0000, 0x1082, 0x2104, 0x3186, 0x4228, 0x52AA, 0x632C, 0x73AE,
    0x8C51, 0x9CD3, 0xAD55, 0xBDD7, 0xCE79, 0xDEFB, 0xEF7D, 0xFFFF,
] + [0] * 224

# register offsets from 0x3A000000
BORDER_COLOR, BG_COLOR, FONT_CTRL, TILE_SCROLL = 0x104, 0x108, 0x110, 0x11C
BORDER_DIMS = 0x130
TILE_PRIORITY, TILES_ENABLE, GFX_ENABLE, PALETTE = 0x124, 0x12C, 0x134, 0x154
GFX_PTR, GFX_SCROLL, TILEMAP_PTR = 0x158, 0x15C, 0x16C
VIDEO_MODE = 0x298
PAL_SEL, BLIT_CBASE, BLIT_LUT = 0x294, 0x1D0, 0x1D4
HIRES_DX = 24            # dots
BLIT_SRC, BLIT_DST, BLIT_DIMS, BLIT_MOD, BLIT_CTRL = 0x1A0, 0x1A4, 0x1A8, 0x1AC, 0x1B0
MAP0, MAP1 = 0x38A00, 0x40200
SHOT = 0x1FFFF0          # emulator-only: a write takes a screenshot


def rgb565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return (r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31


def load_font(path):
    if not path:                       # no font: text renders blank (blitter tests)
        return bytes(256 * 16)
    rows = [int(l.split()[0], 16) for l in open(path) if l.strip() and not l.startswith("//")]
    return bytes(rows)


def write_png(path, w, h, rgb):
    raw = b"".join(b"\0" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


class Screen:
    def __init__(self, uc, regs, font_path):
        self.uc, self.regs = uc, regs          # regs: vxemu's dict of MMIO writes
        self.pal = [list(DEFAULT_PALETTE) for _ in range(8)]   # 8 x 256 (core v0.95)
        self.lut, self.lut_on = list(range(256)), False
        self.font = load_font(font_path)

    def write(self, off, value):
        """MMIO write hook; returns True if handled here."""
        if off == PALETTE:
            self.pal[(value >> 24) & 7][(value >> 16) & 0xFF] = value & 0xFFFF
        elif off == BLIT_LUT:
            if value & 0x80000000:
                self.lut_on = bool(value & 1)
            else:
                self.lut[(value >> 8) & 0xFF] = value & 0xFF
        elif off == BLIT_CTRL and value & 1:
            self.blit(value)
        return False

    def blit(self, ctrl):
        r, uc = self.regs, self.uc
        w, h = r.get(BLIT_DIMS, 0) & 0xFFFF, r.get(BLIT_DIMS, 0) >> 16
        smod, dmod = r.get(BLIT_MOD, 0) & 0xFFFF, r.get(BLIT_MOD, 0) >> 16
        dst = CHIP + (r.get(BLIT_DST, 0) & 0xFFFFFF)
        src = CHIP + (r.get(BLIT_SRC, 0) & 0xFFFFFF)
        key = (ctrl >> 8) & 0xFF
        if ctrl & 0x10:                          # LINE: Bresenham, |dx| = width, dy = height
            self.line(dst, w, h, dmod, -1 if ctrl & 0x20 else 1, key)
            return
        if ctrl & 0xE8 or ctrl & 0x30000:       # XOR/TEX*/GOURAUD: not modelled
            return
        for y in range(h):
            d = dst + y * (w + dmod)
            if ctrl & 2:                         # FILL
                uc.mem_write(d, bytes([key]) * w)
                continue
            row = bytes(uc.mem_read(src + y * (w + smod), w))
            cbase = r.get(BLIT_CBASE, 0) & 0xFF
            if ctrl & 4 or cbase or self.lut_on:  # key on the raw byte, then + cbase, then the LUT
                old = bytearray(uc.mem_read(d, w))
                for i, p in enumerate(row):
                    if not (ctrl & 4 and p == key):
                        q = (p + cbase) & 0xFF
                        old[i] = self.lut[q] if self.lut_on else q
                row = bytes(old)
            uc.mem_write(d, row)

    def line(self, dst, dx, dy, stride, sx, colour):
        x = y = 0
        err = dx - dy
        c = bytes([colour])
        for _ in range(max(dx, dy) + 1):
            self.uc.mem_write(dst + y * stride + x * sx, c)
            e2 = 2 * err
            if e2 > -dy:
                err -= dy
                x += 1
            if e2 < dx:
                err += dx
                y += 1

    def render(self, path):
        r = self.regs
        ps = r.get(PAL_SEL, 0)                   # palette per layer: picture, tiles, border
        pal_g = [bytes(rgb565(v)) for v in self.pal[ps & 7]]
        pal_t = [bytes(rgb565(v)) for v in self.pal[(ps >> 4) & 7]]
        pal_b = [bytes(rgb565(v)) for v in self.pal[(ps >> 12) & 7]]
        bg = r.get(BG_COLOR, 6) & 0xFF
        gfx_on = r.get(GFX_ENABLE, 0) & 1
        tiles_on = r.get(TILES_ENABLE, 1) & 1
        tiles_top = r.get(TILE_PRIORITY, 1) & 1
        fb = bytes(self.uc.mem_read(CHIP + (r.get(GFX_PTR, 0) & 0x1FFF) * 4096, 512 * 256)) if gfx_on else b""
        gs = r.get(GFX_SCROLL, 0)
        gx, gy = gs & 0x1FF, (gs >> 16) & 0xFF   # column x lands on screen x (core v0.95+)
        ts = r.get(TILE_SCROLL, 0)
        tx, ty = ts & 0x1FF, (ts >> 16) & 0x1FF
        base = r.get(TILEMAP_PTR, 0) & 0x7F
        bd = r.get(BORDER_DIMS, 0)
        bl, br = bd & 0xFF, (bd >> 8) & 0xFF
        bt, bb = max(1, (bd >> 16) & 0xFF), max(1, bd >> 24)   # top/bottom 0 is stored as 1 (v0.78)
        border = pal_b[r.get(BORDER_COLOR, 14) & 0xFF]
        font = self.font
        hires = r.get(VIDEO_MODE, 0) & 1
        ow = 2 * W if hires else W           # output width: dots in hi-res
        img = bytearray(ow * H * 3)
        for y in range(H):
            if hires:                         # text row R = map rows 2R, 2R + 1: 128 cells
                trow = (base // 2 + ((y + ty) >> 3)) & 0x3F
                win = (MAP0 + trow * 512) if trow < 32 else (MAP1 + (trow - 32) * 512)
            else:
                trow = (base + ((y + ty) >> 3)) & 0x7F
                win = (MAP0 + trow * 256) if trow < 64 else (MAP1 + (trow - 64) * 256)
            py = (y + ty) & 7
            frow = ((y + gy) & 0xFF) * 512
            for xo in range(ow):
                x = xo >> 1 if hires else xo     # pixel, for border and GFX
                o = (y * ow + xo) * 3
                if x < bl or x >= W - br or y < bt or y >= H - bb:
                    img[o:o + 3] = border
                    continue
                t = 0
                if tiles_on:
                    mx = xo + tx + (HIRES_DX if hires else 0)
                    tcol = (mx >> 3) & (127 if hires else 63)
                    cell = r.get(win + tcol * 4, 0)
                    ch = (cell & 0xFF) | ((cell >> 18) & 0x100)
                    bits = font[(ch & 0xFF) * 16 + (7 - py if cell & (1 << 25) else py)]   # ROM: 256 chars x 16 rows
                    px = mx & 7
                    if cell & (1 << 24):
                        px = 7 - px
                    t = (cell >> 8) & 0xFF if (bits >> (7 - px)) & 1 else (cell >> 16) & 0xFF
                g = fb[frow + ((x + gx) & 511)] if gfx_on else 0
                if tiles_top:
                    c = pal_t[t] if t else pal_g[g or bg]
                else:
                    c = pal_g[g] if g else (pal_t[t] if t else pal_g[bg])
                img[o:o + 3] = c
        write_png(path, ow, H, img)
        return path

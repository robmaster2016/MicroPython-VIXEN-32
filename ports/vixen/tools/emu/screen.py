# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
"""Display model for vxemu: VIH registers, palette, tile layer, GFX layer and
the blitter's FILL/COPY/LINE, rendered to a PNG at the VIXEN-32's native 384x224.

Used by vxemu.py when VX_SCREEN is set. Not modelled: sprites (the LVGL mouse
cursor), the border (the port runs borderless), 8x16 and uploaded fonts,
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
TILE_PRIORITY, TILES_ENABLE, GFX_ENABLE, PALETTE = 0x124, 0x12C, 0x134, 0x154
GFX_PTR, GFX_SCROLL, TILEMAP_PTR = 0x158, 0x15C, 0x16C
BLIT_SRC, BLIT_DST, BLIT_DIMS, BLIT_MOD, BLIT_CTRL = 0x1A0, 0x1A4, 0x1A8, 0x1AC, 0x1B0
MAP0, MAP1 = 0x38A00, 0x40200
SHOT = 0x1FFFF0          # emulator-only: a write takes a screenshot


def rgb565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return (r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31


def load_font(path):
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
        self.pal = list(DEFAULT_PALETTE)
        self.font = load_font(font_path)

    def write(self, off, value):
        """MMIO write hook; returns True if handled here."""
        if off == PALETTE:
            self.pal[(value >> 16) & 0xFF] = value & 0xFFFF
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
            if ctrl & 4:                         # colour key
                old = bytearray(uc.mem_read(d, w))
                for i, p in enumerate(row):
                    if p != key:
                        old[i] = p
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
        pal = [rgb565(v) for v in self.pal]
        bg = r.get(BG_COLOR, 6) & 0xFF
        gfx_on = r.get(GFX_ENABLE, 0) & 1
        tiles_on = r.get(TILES_ENABLE, 1) & 1
        tiles_top = r.get(TILE_PRIORITY, 1) & 1
        fb = bytes(self.uc.mem_read(CHIP + (r.get(GFX_PTR, 0) & 0x1FFF) * 4096, 512 * 256)) if gfx_on else b""
        gs = r.get(GFX_SCROLL, 0)
        gx, gy = (gs & 0x1FF) & ~7, (gs >> 16) & 0xFF   # fine X is the hardware's own offset
        ts = r.get(TILE_SCROLL, 0)
        tx, ty = ts & 0x1FF, (ts >> 16) & 0x1FF
        base = r.get(TILEMAP_PTR, 0) & 0x7F
        font = self.font
        img = bytearray(W * H * 3)
        for y in range(H):
            trow = (base + ((y + ty) >> 3)) & 0x7F
            win = (MAP0 + trow * 256) if trow < 64 else (MAP1 + (trow - 64) * 256)
            py = (y + ty) & 7
            frow = ((y + gy) & 0xFF) * 512
            for x in range(W):
                t = 0
                if tiles_on:
                    cell = r.get(win + (((x + tx) >> 3) & 63) * 4, 0)
                    ch = (cell & 0xFF) | ((cell >> 18) & 0x100)
                    bits = font[(ch & 0xFF) * 16 + (7 - py if cell & (1 << 25) else py)]   # ROM: 256 chars x 16 rows
                    px = (x + tx) & 7
                    if cell & (1 << 24):
                        px = 7 - px
                    t = (cell >> 8) & 0xFF if (bits >> (7 - px)) & 1 else (cell >> 16) & 0xFF
                g = fb[frow + ((x + gx) & 511)] if gfx_on else 0
                if tiles_top:
                    c = t or g or bg
                else:
                    c = g or t or bg
                o = (y * W + x) * 3
                img[o:o + 3] = bytes(pal[c])
        write_png(path, W, H, img)
        return path

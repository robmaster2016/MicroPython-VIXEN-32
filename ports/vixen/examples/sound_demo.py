# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# sound_demo.py -- music you can see. WAV and SID playback, drawn as art.
#
#   import sound_demo                    # starts the show: the SID, then the jazz WAV
#   sound_demo.art()                     # once more (a second import does nothing)
#   sound_demo.art("neon_grid.sid")      # one file (WAV or SID), from examples/ or a path
#   sound_demo.play_all("/sd/music")     # plain playback of a folder, console only
#
# A WAV becomes a "rosette": the real samples, read live from the sound
# module's buffer, bend rings that ripple outward. A SID tune becomes a
# "harmonograph": every voice draws a Lissajous figure whose shape follows
# the note it plays, growing with the gate and fading with the release. The
# SID registers come from the player's register shadow in chip RAM.
#
# The pictures are drawn the way a crack intro would: by the blitter's
# hardware line engine, straight into the three framebuffers, which the demo
# flips itself. LVGL only draws the background and the titles once. The
# pictures need the LVGL build (its palette and framebuffers); without it
# art() just plays. RUN/STOP
# (Ctrl-C) ends; the music stops with it. Music in examples/: "aurec-jazz.wav"
# and "neon_grid.sid" ("Neon Grid", written for this project, MIT;
# its source is tools/sidtune/compose.py).

import math
import os
import sound
import time

try:
    import uctypes
except ImportError:
    uctypes = None

HERE = __file__.rsplit("/", 1)[0] if "/" in __file__ else "."
SHOW = ("neon_grid.sid", "aurec-jazz.wav")

# Where the sound module keeps its data (vixen_sound.c): chip RAM window
# SND_BASE = 0x380000; the SID register shadow at +0x10400, WAV data at +0x11000.
CHIP = 0x40000000
SID_REGS = CHIP + 0x380000 + 0x10400
WAV_BASE = CHIP + 0x380000 + 0x11000
WAV_MAX = 0x7F0000 - 0x391000
PCM_MAX_RATE = 48007

W, H = 384, 224
SIN = [int(1024 * math.sin(2 * math.pi * i / 256)) for i in range(256)]


def _path(name):
    return name if "/" in name else HERE + "/" + name


# ---- plain playback ---------------------------------------------------------

def play_file(name, seconds=30):
    low = name.lower()
    if low.endswith(".sid"):
        sound.sid.play(name)
        playing = sound.sid.playing
    elif low.endswith(".wav"):
        sound.wav.play(name)
        playing = sound.wav.playing
    else:
        return
    print("playing", name)
    t = time.ticks_ms()
    # time.sleep() keeps the SID player running (as any Python code does)
    while playing() and time.ticks_diff(time.ticks_ms(), t) < seconds * 1000:
        time.sleep_ms(100)
    sound.sid.stop()
    sound.wav.stop()


def play_all(folder="music", seconds=30):
    try:
        for name in sorted(os.listdir(folder)):
            play_file(folder + "/" + name, seconds)
    finally:
        sound.sid.stop()
        sound.wav.stop()



# ---- file headers ----------------------------------------------------------

def _wav_info(path):
    """How the sound module stores this WAV: (channels, rate, 16 bit?,
    samples per channel, channel spacing, bits in the file)."""
    with open(path, "rb") as f:
        head = f.read(4096)
    pos, ch, rate, bits, data = 12, 1, 22050, 8, 0
    while pos + 8 <= len(head):
        tag, size = head[pos:pos + 4], int.from_bytes(head[pos + 4:pos + 8], "little")
        if tag == b"fmt ":
            ch = int.from_bytes(head[pos + 10:pos + 12], "little")
            rate = int.from_bytes(head[pos + 12:pos + 16], "little")
            bits = int.from_bytes(head[pos + 22:pos + 24], "little")
        elif tag == b"data":
            data = size
            break
        pos += 8 + size + (size & 1)
    decim = (rate + PCM_MAX_RATE - 1) // PCM_MAX_RATE
    frames = data // (ch * bits // 8) // decim
    cap = (WAV_MAX // ch) & ~3
    out16 = bits > 8 and frames <= cap // 2
    return ch, rate // decim, out16, frames, cap, bits


def _sid_info(path):
    with open(path, "rb") as f:
        h = f.read(0x7C)

    def text(o):
        return h[o:o + 32].split(b"\0")[0].decode()

    ntsc = len(h) > 0x77 and (h[0x77] >> 2) & 3 == 2
    return text(0x16), text(0x36), text(0x56), 1022727 if ntsc else 985248


# ---- the pictures ------------------------------------------------------------

class _Stage:
    def __init__(self, lv):
        import vixen
        try:
            vixen.lv_display()
        except RuntimeError:
            pass                       # already set up by an earlier program
        self.lv = lv
        scr = lv.obj()
        scr.remove_flag(lv.obj.FLAG.SCROLLABLE)
        # plain black: a gradient falls apart into bands in 256 colours
        scr.set_style_bg_color(lv.color_hex(0x000000), 0)
        self.scr = scr
        self.title = lv.label(scr)
        self.title.set_style_text_font(lv.font_montserrat_12, 0)
        self.title.set_style_text_color(lv.color_hex(0xE8E4D8), 0)
        self.title.set_pos(10, H - 32)
        self.sub = lv.label(scr)
        self.sub.set_style_text_font(lv.font_montserrat_10, 0)
        self.sub.set_style_text_color(lv.color_hex(0x7A86A8), 0)
        self.sub.set_pos(10, H - 16)
        lv.screen_load(scr)

    def labels(self, title, sub):
        self.title.set_text(title)
        self.sub.set_text(sub)
        self.title.move_foreground()
        self.sub.move_foreground()


# ---- drawing with the blitter --------------------------------------------------
BLIT_DST, BLIT_DIMS, BLIT_MOD, BLIT_CTRL, BLIT_STATUS = (
    0x3A0001A4, 0x3A0001A8, 0x3A0001AC, 0x3A0001B0, 0x3A0001B4)
GFX_PTR = 0x3A000158
FBS = (0x300000, 0x320000, 0x340000)   # the LVGL driver's framebuffers (vx_lvgl.c)
STRIDE = 512
DRAW_H = 186                           # rows the pictures use; the titles sit below


def rgb(r, g, b):
    """Palette index of the LVGL build's 6x6x6 colour cube (0-5 each)."""
    return r * 36 + g * 6 + b


# twelve saturated colours around the wheel
WHEEL = tuple(rgb(*c) for c in (
    (5, 0, 0), (5, 2, 0), (5, 4, 0), (4, 5, 0), (2, 5, 0), (0, 5, 1),
    (0, 5, 4), (0, 4, 5), (0, 2, 5), (1, 0, 5), (3, 0, 5), (5, 0, 4)))


def dim(index, level):
    """The cube colour index at level/5 of its brightness."""
    r, g, b = index // 36, index // 6 % 6, index % 6
    return rgb(r * level // 5, g * level // 5, b * level // 5)


class _Gfx:
    """Triple-buffered line drawing on the blitter. LVGL renders the screen
    (background and titles) into all three framebuffers first; then each
    frame clears the picture area of the next buffer, draws, and shows it."""

    def __init__(self, lv, scr):
        import machine
        self.m = machine.mem32
        for _ in range(3):             # every buffer gets the full screen
            scr.invalidate()
            lv.refr_now(None)
        self.k = 0
        self.fb = FBS[0]

    def _idle(self):
        m = self.m
        while m[BLIT_STATUS] & 3:
            pass

    def clear(self):
        m = self.m
        self.k = (self.k + 1) % 3      # neither shown nor waiting to be shown
        self.fb = FBS[self.k]
        self._idle()
        m[BLIT_MOD] = (STRIDE - W) << 16
        m[BLIT_DST] = self.fb
        m[BLIT_DIMS] = (DRAW_H << 16) | W
        m[BLIT_CTRL] = 0x03            # fill with colour 0
        self._idle()
        m[BLIT_MOD] = STRIDE << 16

    def poly(self, xs, ys, n, colour):
        """Lines through the first n points (already clipped to the area)."""
        m = self.m
        fb = self.fb
        go = (colour << 8) | 0x11      # line, go
        x0, y0 = xs[0], ys[0]
        for i in range(1, n):
            x1, y1 = xs[i], ys[i]
            if y0 <= y1:
                dst, dy, dx = fb + y0 * STRIDE + x0, y1 - y0, x1 - x0
            else:
                dst, dy, dx = fb + y1 * STRIDE + x1, y0 - y1, x0 - x1
            while m[BLIT_STATUS] & 2:  # one blit may wait in the queue
                pass
            m[BLIT_DST] = dst
            if dx < 0:
                m[BLIT_DIMS] = (dy << 16) | -dx
                m[BLIT_CTRL] = go | 0x20
            else:
                m[BLIT_DIMS] = (dy << 16) | dx
                m[BLIT_CTRL] = go
            x0, y0 = x1, y1

    def show(self):
        self._idle()
        self.m[GFX_PTR] = self.fb >> 12   # latched at the next frame start


def _clip(v, hi):
    return 0 if v < 0 else (hi if v > hi else v)


class _Rosette:
    """WAV: rings bent by the live samples, rippling outward and fading."""
    N = 36          # points per ring
    RINGS = 4

    def __init__(self, gfx, info):
        self.g = gfx
        self.ch, self.rate, self.is16, self.frames, cap, _ = info
        size = self.frames * (2 if self.is16 else 1)
        self.left = uctypes.bytearray_at(WAV_BASE, size)
        self.right = uctypes.bytearray_at(WAV_BASE + (cap if self.ch == 2 else 0), size)
        self.hist = []
        self.xs = [0] * (self.N + 1)
        self.ys = [0] * (self.N + 1)
        self.hue = 0

    def frame(self, t_ms):
        g = self.g
        n0 = (t_ms * self.rate // 1000) % self.frames
        step = max(1, self.rate // 2400)
        N, frames, is16 = self.N, self.frames, self.is16
        left, right = self.left, self.right
        shape = [0] * N
        loud = 0
        k = n0
        for i in range(N):
            v = (left if i & 1 else right)[2 * k + 1 if is16 else k]
            if v > 127:
                v -= 256
            loud += v if v > 0 else -v
            shape[i] = v
            k += step
            if k >= frames:
                k -= frames
        loud //= N
        hist = self.hist
        hist.insert(0, shape)
        if len(hist) > self.RINGS:
            hist.pop()
        self.hue += 1 + loud // 12
        hue = self.hue // 8
        rot = (t_ms // 40) & 255
        cx, cy = W // 2, DRAW_H // 2
        da = 256 // N
        xs, ys = self.xs, self.ys
        g.clear()
        for age in range(len(hist) - 1, -1, -1):        # oldest first, newest on top
            sh = hist[age]
            base = 16 + age * 15
            amp = 26 + age * 6
            a = (rot + age * 9) & 255
            for i in range(N + 1):
                r = base + sh[i if i < N else 0] * amp // 128
                xs[i] = _clip(cx + SIN[(a + 64) & 255] * r // 1024, W - 1)
                ys[i] = _clip(cy + SIN[a] * r * 11 // 16384, DRAW_H - 1)
                a = (a + da) & 255
            g.poly(xs, ys, N + 1, dim(WHEEL[(hue + age) % 12], 5 - age))
        # the core: a small polygon that pulses with the loudness
        d = 3 + loud // 6
        for i in range(9):
            a = (i * 32) & 255
            xs[i] = cx + SIN[(a + 64) & 255] * d // 1024
            ys[i] = cy + SIN[a] * d // 1024
        g.poly(xs, ys, 9, WHEEL[(hue + 6) % 12])
        g.show()


# semitone -> frequency ratio of the figure: consonant ones look calm, the
# others weave dense patterns
RATIOS = ((1, 1), (3, 5), (2, 3), (4, 5), (1, 2), (3, 4), (2, 5), (1, 3), (3, 2), (5, 4), (4, 3), (2, 1))
VOICE_COLOUR = (rgb(5, 2, 0), rgb(0, 5, 5), rgb(5, 0, 5))


class _Harmonograph:
    """SID: one Lissajous figure per voice, shaped by its note, with a trail."""
    N = 48
    CX = (72, 192, 312)         # the three voices side by side

    def __init__(self, gfx, clock):
        self.g = gfx
        self.regs = uctypes.bytearray_at(SID_REGS, 0x19)
        self.unit = clock / 16777216
        self.env = [0, 0, 0]
        self.phase = [0, 85, 170]
        self.freq = [-1, -1, -1]
        self.note = [0, 0, 0]
        self.t = [i * 256 // (self.N - 1) for i in range(self.N)]
        self.cur = [([0] * self.N, [0] * self.N) for _ in range(3)]
        self.old = [([0] * self.N, [0] * self.N) for _ in range(3)]

    def frame(self, t_ms):
        g = self.g
        r = self.regs
        vol = r[0x18] & 15
        cy = DRAW_H // 2
        tt = self.t
        N = self.N
        g.clear()
        for v in range(3):
            o = v * 7
            freq = r[o] | (r[o + 1] << 8)
            ctrl = r[o + 4]
            # envelope: quick attack while the gate is on, slow release after
            e = self.env[v]
            e = e + (100 - e) // 3 if ctrl & 1 and freq else e * 7 // 8
            self.env[v] = e
            if freq != self.freq[v]:            # the logarithm only when the note changes
                self.freq[v] = freq
                hz = freq * self.unit
                self.note[v] = int(17.31234 * math.log(hz / 16.352) + 0.5) if hz > 20 else 0
            note = self.note[v]
            a, b = RATIOS[note % 12]
            size = (10 + 46 * e // 100) * (8 + vol) // 23
            ph = self.phase[v] = (self.phase[v] + 2 + note // 12) & 255
            cx = self.CX[v]
            # last frame's figure becomes the trail
            self.cur[v], self.old[v] = self.old[v], self.cur[v]
            xo, yo = self.old[v]
            if e:
                g.poly(xo, yo, N, dim(VOICE_COLOUR[v], 2))
            xs, ys = self.cur[v]
            shiver = ctrl & 0x80
            for i in range(N):
                t = tt[i]
                x = cx + SIN[(a * t + ph) & 255] * size // 1024
                if shiver:
                    x += SIN[(i * 97 + t_ms) & 255] >> 8
                xs[i] = x
                ys[i] = cy + SIN[(b * t + 64) & 255] * size // 1024
            g.poly(xs, ys, N, dim(VOICE_COLOUR[v], 2 + 3 * e // 100))
        g.show()


SID_SECONDS = 62      # a SID tune loops forever: two passes of Neon Grid


def _length(path, seconds):
    if seconds:
        return seconds
    return SID_SECONDS if path.lower().endswith(".sid") else 3600   # a WAV ends by itself


def art(*names, seconds=None, fps=25):
    """Play WAV/SID files with pictures (LVGL build). seconds=None: a WAV to
    its end, a SID for SID_SECONDS."""
    names = names or SHOW
    try:
        import lvgl as lv
    except ImportError:
        lv = None
    if lv is None or uctypes is None:
        for n in names:
            play_file(_path(n), _length(n, seconds))
        return
    stage = None
    try:
        for name in names:
            path = _path(name)
            old = stage
            stage = _Stage(lv)
            if old:
                old.scr.delete()
            if path.lower().endswith(".wav"):
                info = _wav_info(path)
                stage.labels(name.rsplit("/", 1)[-1], "%d Hz %s, %d bit" % (
                    info[1], "stereo" if info[0] == 2 else "mono", info[5]))
                pic = _Rosette(_Gfx(lv, stage.scr), info)
                sound.wav.play(path)
                playing = sound.wav.playing
            else:
                title, author, released, clock = _sid_info(path)
                stage.labels(title, "%s  -  %s" % (author, released))
                pic = _Harmonograph(_Gfx(lv, stage.scr), clock)
                sound.sid.play(path)
                playing = sound.sid.playing
            t0 = time.ticks_ms()
            frame_ms = 1000 // fps
            limit = _length(path, seconds) * 1000
            while playing():
                t = time.ticks_diff(time.ticks_ms(), t0)
                if t > limit:
                    break
                pic.frame(t)
                wait = frame_ms - (time.ticks_diff(time.ticks_ms(), t0) - t)
                time.sleep_ms(wait if wait > 1 else 1)   # the SID player runs meanwhile
            sound.sid.stop()
            sound.wav.stop()
    finally:
        sound.sid.stop()
        sound.wav.stop()
        if stage:                     # hand the display back to LVGL
            stage.scr.invalidate()
            lv.refr_now(None)


# "import sound_demo" is all it takes. A program that only wants the
# functions puts a "sound_demo_quiet" entry into sys.modules first.
import sys
if "sound_demo_quiet" not in sys.modules:
    art()

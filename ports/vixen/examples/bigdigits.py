# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# bigdigits.py -- large seven-segment numbers for LVGL screens.
#
# The firmware carries Montserrat 10/12/14 only; for a clock or a big
# temperature this draws segment digits from plain LVGL rectangles instead.
# Used by clock.py and weather.py.
#
#   import bigdigits
#   d = bigdigits.Display(lv.screen_active(), x=None, y=40, height=60, layout="88:88")
#   d.set("12:34")            # digits, ' ', '-' and 'o' (a degree sign)

import lvgl as lv

#            a  b  c  d  e  f  g
_SEG = {
    "0": (1, 1, 1, 1, 1, 1, 0), "1": (0, 1, 1, 0, 0, 0, 0),
    "2": (1, 1, 0, 1, 1, 0, 1), "3": (1, 1, 1, 1, 0, 0, 1),
    "4": (0, 1, 1, 0, 0, 1, 1), "5": (1, 0, 1, 1, 0, 1, 1),
    "6": (1, 0, 1, 1, 1, 1, 1), "7": (1, 1, 1, 0, 0, 0, 0),
    "8": (1, 1, 1, 1, 1, 1, 1), "9": (1, 1, 1, 1, 0, 1, 1),
    "-": (0, 0, 0, 0, 0, 0, 1), " ": (0, 0, 0, 0, 0, 0, 0),
    "o": (1, 1, 0, 0, 0, 1, 1),     # degree sign: the upper square
}


def _box(parent, x, y, w, h, color):
    o = lv.obj(parent)
    o.remove_style_all()
    o.set_style_bg_opa(lv.OPA.COVER, 0)
    o.set_style_bg_color(color, 0)
    o.set_style_radius(1, 0)
    o.set_pos(x, y)
    o.set_size(w, h)
    return o


class Digit:
    def __init__(self, parent, x, y, h, color, t):
        w = h // 2
        m = h // 2
        self.segs = (
            _box(parent, x + t, y, w - 2 * t, t, color),                 # a
            _box(parent, x + w - t, y + t, t, m - t - t // 2, color),    # b
            _box(parent, x + w - t, y + m + t // 2, t, m - t - t // 2, color),  # c
            _box(parent, x + t, y + h - t, w - 2 * t, t, color),         # d
            _box(parent, x, y + m + t // 2, t, m - t - t // 2, color),   # e
            _box(parent, x, y + t, t, m - t - t // 2, color),            # f
            _box(parent, x + t, y + m - t // 2, w - 2 * t, t, color),    # g
        )
        self.ch = None

    def set(self, ch):
        if ch == self.ch:
            return
        self.ch = ch
        for seg, on in zip(self.segs, _SEG.get(ch, _SEG[" "])):
            if on:
                seg.remove_flag(lv.obj.FLAG.HIDDEN)
            else:
                seg.add_flag(lv.obj.FLAG.HIDDEN)


class Display:
    """A row of digits. ':' and '.' take a narrow cell of their own.

    layout is a template such as "88:88" (default: chars digits); x=None
    centres the row in a 384 pixel wide parent."""

    def __init__(self, parent, x, y, height, chars=4, color=None, layout=None):
        color = color or lv.color_hex(0x40C0FF)
        layout = layout or "8" * chars
        t = max(2, height // 9)
        gap = max(3, height // 8)
        self.width = sum(t if c in ":." else height // 2 for c in layout) + gap * (len(layout) - 1)
        if x is None:
            x = (384 - self.width) // 2
        self.x = x
        self.cells = []
        for c in layout:
            if c in ":.":
                dots = []
                if c == ":":
                    dots.append(_box(parent, x, y + height // 3 - t // 2, t, t, color))
                    dots.append(_box(parent, x, y + (2 * height) // 3 - t // 2, t, t, color))
                else:
                    dots.append(_box(parent, x, y + height - t, t, t, color))
                self.cells.append(dots)
                x += t + gap
            else:
                self.cells.append(Digit(parent, x, y, height, color, t))
                x += height // 2 + gap

    def set(self, text):
        digits = [cell for cell in self.cells if isinstance(cell, Digit)]
        text = [c for c in text if c not in ":."]
        for cell, ch in zip(digits, text):
            cell.set(ch)

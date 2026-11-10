# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# Mouse test for the VIXEN-32 MicroPython port (works in both builds).
#
#     import mouse_test           # examples/ is on sys.path
#
# Prints the mouse register (VIH_MOUSE, 0x3A0001EC) whenever it changes, for
# 30 seconds or until RUN/STOP / Ctrl-C. The VIXEN core decodes an AMIGA mouse
# on joystick port 1 directly from the pins:
#
#   x, y        8-bit counters, they change while the mouse moves
#   L R M       buttons (a 2-button mouse never shows M)
#   pins        the raw pin levels V H VQ HQ POT0 POT1 FIRE, live
#
# What to look for:
#   - counters change when moving        -> mouse works, LVGL will see it
#   - only the pins toggle, no counting   -> signals arrive but are not
#                                           Amiga quadrature (e.g. an Atari ST
#                                           mouse: two pins swapped)
#   - nothing changes at all              -> nothing reaches the port: a PS/2
#                                           mouse needs an ACTIVE PS/2-to-Amiga
#                                           converter, a plain plug adapter
#                                           cannot translate the protocol

import time

import machine

MOUSE = 0x3A0001EC
PIN_NAMES = ("V", "H", "VQ", "HQ", "POT0", "POT1", "FIRE")

print("mouse test: move the mouse and press its buttons (30 s, RUN/STOP ends)")
last = None
end = time.ticks_add(time.ticks_ms(), 30000)
while time.ticks_diff(end, time.ticks_ms()) > 0:
    v = machine.mem32[MOUSE] & 0xFFFFFFFF
    if v != last:
        pins = " ".join("%s=%d" % (n, (v >> (19 + i)) & 1) for i, n in enumerate(PIN_NAMES))
        print("x=%3d y=%3d  L=%d R=%d M=%d  %s" % (
            v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 1, (v >> 17) & 1, (v >> 18) & 1, pins))
        last = v
    time.sleep_ms(20)
print("mouse test: done")

# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# vx: examples   (run_tests.sh puts ports/vixen/examples on a RAM /sd first)
# The examples with live data: sysinfo, clock (NTP), weather (Open-Meteo),
# wget (HTTPS to the SD card), and sound_demo with the SID. Needs the internet.
import os, gc

import sysinfo                      # prints its report on import

import clock
clock.sync()
t = clock.now()
print("clock", t)
assert t[0] >= 2026 and 1 <= t[1] <= 12 and 0 <= t[3] < 24, t

import weather
w = weather.fetch()
weather._text(w, "Berlin")
assert len(w["daily"]["time"]) == 4 and "temperature_2m" in w["current"]

import wget
p = wget.get("https://micropython.org/static/img/Mlogo_138wh.png")
size = os.stat(p)[6]
with open(p, "rb") as f:
    head = f.read(8)
print("wget", p, size)
assert size > 100 and head == b"\x89PNG\r\n\x1a\n", (size, head)

try:
    import lvgl
except ImportError:
    lvgl = None
if lvgl:                            # the LVGL screens build and update
    weather.show()
    clock.start()
    clock._tick()
    gc.collect()
    print("lvgl screens ok, mem_free", gc.mem_free())
import sys
sys.modules["sound_demo_quiet"] = True   # import without starting the whole show
import sound_demo                   # SID with the harmonograph (LVGL) or plain
sound_demo.art("neon_grid.sid", seconds=2)
print("TEST OK")

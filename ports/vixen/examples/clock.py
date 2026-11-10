# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# clock.py -- a big clock with the time from the internet (NTP).
#
#   import clock
#   clock.start()            # LVGL build: full-screen clock, runs in the background
#   clock.now()              # (year, month, day, hour, minute, second, weekday) Berlin time
#
# The VIXEN-32 has no real-time clock: clock.sync() asks an NTP server once
# and the time then runs on time.ticks_ms() (re-synced every hour). Shows
# Berlin time with the EU summer-time rule; set OFFSET_H/DST for elsewhere.
# The LVGL screen needs bigdigits.py next to this file; without LVGL,
# clock.start() prints the time once a second.

import socket
import struct
import time

SERVER = "pool.ntp.org"
OFFSET_H = 1          # CET
DST = True            # EU rule: +1 h from the last Sunday in March to the last in October
DAYS = ("Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday")
MONTHS = ("January", "February", "March", "April", "May", "June", "July",
          "August", "September", "October", "November", "December")

_base = None          # (unix seconds, ticks_ms) at the last sync


def sync(server=SERVER):
    """Fetch the time from an NTP server. Returns the UTC unix time."""
    global _base
    import network
    if not network.LAN().isconnected():
        import net
        net.up()
    addr = socket.getaddrinfo(server, 123)[0][-1]
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.settimeout(3)
        q = bytearray(48)
        q[0] = 0x1B                      # version 3, client
        t0 = time.ticks_ms()
        s.sendto(q, addr)
        d = s.recv(48)
        rtt = time.ticks_diff(time.ticks_ms(), t0)
    finally:
        s.close()
    secs, frac = struct.unpack("!II", d[40:48])
    ms = (frac * 1000) >> 32
    unix = secs - 2208988800
    # the moment the answer was sent, plus half the round trip
    _base = (unix, time.ticks_add(time.ticks_ms(), -(ms + rtt // 2)))
    return unix


def _days_to_date(z):
    # days since 1970-01-01 -> (y, m, d), proleptic Gregorian (H. Hinnant)
    z += 719468
    era = z // 146097
    doe = z - era * 146097
    yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    y = yoe + era * 400
    doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
    mp = (5 * doy + 2) // 153
    d = doy - (153 * mp + 2) // 5 + 1
    m = mp + 3 if mp < 10 else mp - 9
    return (y + 1 if m <= 2 else y), m, d


def _last_sunday(y, m):
    # unix day of the last Sunday of month m (March or October)
    last = _date_to_days(y, m + 1, 1) - 1
    return last - (last + 4) % 7          # 1970-01-01 was a Thursday (4, Sunday = 0)


def _date_to_days(y, m, d):
    y -= m <= 2
    era = y // 400
    yoe = y - era * 400
    doy = (153 * (m - 3 if m > 2 else m + 9) + 2) // 5 + d - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468


def utc():
    """UTC unix seconds (float), from the last sync."""
    if _base is None:
        sync()
    return _base[0] + time.ticks_diff(time.ticks_ms(), _base[1]) / 1000


def now():
    t = int(utc())
    local = t + OFFSET_H * 3600
    if DST:
        y = _days_to_date(t // 86400)[0]
        start = _last_sunday(y, 3) * 86400 + 3600       # 01:00 UTC
        end = _last_sunday(y, 10) * 86400 + 3600
        if start <= t < end:
            local += 3600
    days, s = divmod(local, 86400)
    y, m, d = _days_to_date(days)
    return y, m, d, s // 3600, s // 60 % 60, s % 60, (days + 3) % 7   # weekday 0 = Monday


# ---- LVGL screen ------------------------------------------------------------
_ui = {}


def _build(lv):
    import vixen
    import bigdigits
    try:
        vixen.lv_display()
    except RuntimeError:
        pass                # already set up by an earlier program
    scr = lv.obj()
    scr.set_style_bg_color(lv.color_hex(0x000000), 0)
    scr.remove_flag(lv.obj.FLAG.SCROLLABLE)
    hm = bigdigits.Display(scr, None, 40, 96, layout="88:88", color=lv.color_hex(0x33E07A))
    sec = bigdigits.Display(scr, None, 150, 26, 2, color=lv.color_hex(0x1C8048))

    def label(y, font, color):
        lb = lv.label(scr)
        lb.set_width(384)
        lb.set_y(y)
        lb.set_style_text_align(lv.TEXT_ALIGN.CENTER, 0)
        lb.set_style_text_font(font, 0)
        lb.set_style_text_color(lv.color_hex(color), 0)
        return lb

    date = label(12, lv.font_montserrat_14, 0xFFFFFF)
    info = label(204, lv.font_montserrat_10, 0x5E7A68)
    info.set_text("Berlin  -  synced with %s  -  VIXEN-32" % SERVER)
    lv.screen_load(scr)
    _ui.update(hm=hm, sec=sec, date=date, last=None)


def _tick(timer=None):
    t = now()
    if _ui["last"] == t[5]:
        return
    _ui["last"] = t[5]
    _ui["hm"].set("%02d:%02d" % (t[3], t[4]))
    _ui["sec"].set("%02d" % t[5])
    _ui["date"].set_text("%s, %d %s %d" % (DAYS[t[6]], t[2], MONTHS[t[1] - 1], t[0]))
    if t[4] == 0 and t[5] == 0:
        try:
            sync()
        except OSError:
            pass


def start():
    sync()
    try:
        import lvgl as lv
    except ImportError:
        while True:                       # Ctrl-C ends
            t = now()
            print("%s %d-%02d-%02d %02d:%02d:%02d" % ((DAYS[t[6]][:3],) + t[:6]))
            time.sleep(1)
    if not _ui:
        _build(lv)
        _ui["timer"] = lv.timer_create(_tick, 200, None)
    _tick()

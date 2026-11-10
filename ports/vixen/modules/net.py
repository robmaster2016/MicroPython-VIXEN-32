# This file is part of MicroPython for the VIXEN-32.
#
# The MIT License (MIT)
#
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

# Bring up VIXEN-32's Ethernet the way BASIC's SHIFT+POUND does: settings from
# the card's NETWORK.TXT ("ip: dhcp", or ip/mask/gw/dns for a static setup,
# optional mac), then wait for an address.
#
#   import net
#   lan = net.up()        # prints LINK UP / IP like BASIC, returns the LAN

import network
import time

CONF = "/sd/vixen-32/network.txt"

# The kernel's default MAC (what BASIC shows with no "mac:" line), so the
# router sees the same machine whether BASIC or MicroPython asks for a lease.
# Only used while the core still holds its reset MAC 02:56:49:58:45:4E.
MAC_KERNEL = b"\x02\x56\x33\x32\x00\x01"
MAC_RESET = b"\x02\x56\x49\x58\x45\x4e"


def _conf(path):
    c = {}
    try:
        with open(path) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if ":" in line:
                    k, v = line.split(":", 1)
                    c[k.strip().lower()] = v.strip()
    except OSError:
        pass
    return c


def up(timeout=20, path=CONF, quiet=False):
    c = _conf(path)
    lan = network.LAN()
    if not lan.active():
        if "mac" in c:
            lan.config(mac=bytes(int(b, 16) for b in c["mac"].split(":")))
        elif lan.config("mac") == MAC_RESET:
            lan.config(mac=MAC_KERNEL)
        lan.active(True)
    if not quiet:
        print("ETHERNET ON  MAC", ":".join("%02X" % b for b in lan.config("mac")))

    t0 = time.ticks_ms()
    while lan.status() == 0:
        if time.ticks_diff(time.ticks_ms(), t0) > timeout * 1000:
            raise OSError("no link -- is the cable in?")
        time.sleep_ms(50)
    if not quiet:
        print("LINK UP")

    ip = c.get("ip", "dhcp").lower()
    if ip != "dhcp":
        lan.ifconfig((ip, c.get("mask", "255.255.255.0"),
                      c.get("gw", "0.0.0.0"), c.get("dns", "8.8.8.8")))
    else:
        while not lan.isconnected():
            if time.ticks_diff(time.ticks_ms(), t0) > timeout * 1000:
                raise OSError("DHCP: no answer")
            time.sleep_ms(50)
    if not quiet:
        a = lan.ifconfig()
        print("IP", a[0], " GW", a[2], " DNS", a[3])
    return lan

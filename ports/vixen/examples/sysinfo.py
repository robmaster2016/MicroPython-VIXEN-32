# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# sysinfo.py -- what is this machine doing? Version, memory, drives, network.
#
#   import sysinfo           # prints the report
#   sysinfo.report()         # again, later

import gc
import os
import sys
import time


def _kb(n):
    return "%d KB" % (n // 1024) if n < 10 * 1024 * 1024 else "%.1f MB" % (n / 1048576)


def report():
    gc.collect()
    free, used = gc.mem_free(), gc.mem_alloc()
    print("MicroPython %d.%d.%d on %s" % (sys.implementation.version[:3]
                                          + (sys.implementation._machine,)))
    print("CPU      VexRiscv rv32im, 100 MHz")
    print("Python   heap %s, %s free (%d %%)" % (_kb(free + used), _kb(free),
                                                 100 * free // (free + used)))
    up = time.ticks_ms() // 1000
    print("Uptime   %d:%02d:%02d" % (up // 3600, up // 60 % 60, up % 60))
    for path in ("/sd", "/a", "/b"):
        try:
            st = os.statvfs(path)
        except OSError:
            continue
        total, avail = st[0] * st[2], st[0] * st[3]
        print("Drive    %-4s %s, %s free" % (path, _kb(total), _kb(avail)))
    try:
        import network
        lan = network.LAN()
        if lan.active():
            ip, mask, gw, dns = lan.ifconfig()
            mac = ":".join("%02x" % b for b in lan.config("mac"))
            print("Network  %s  %s" % (ip if lan.isconnected() else "no link", mac))
            print("         gateway %s, dns %s" % (gw, dns))
            rx, tx, drops, crc = lan.status("stats")[:4]
            print("         frames in %d, out %d, drops %d, crc %d" % (rx, tx, drops, crc))
        else:
            print("Network  off (import net; net.up())")
    except ImportError:
        print("Network  not in this build")
    mods = [m for m in ("lvgl", "sound", "curve25519", "sshd", "mip") if _has(m)]
    print("Modules  " + " ".join(mods))


def _has(name):
    try:
        __import__(name)
        return True
    except ImportError:
        return False


report()

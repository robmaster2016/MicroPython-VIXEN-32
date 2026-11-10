# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# wget.py -- download a file from the web straight onto the SD card.
#
#   import wget
#   wget.get("https://micropython.org/static/img/Mlogo_138wh.png")
#   wget.get(url, "/sd/vixen-32/download/logo.png")
#
# Without a path the file lands in /sd/vixen-32/download/ under its own name.
# HTTP and HTTPS; the data is streamed in 4 KB pieces, so files of any size
# fit. Network via net.up().

import time
import requests

FOLDER = "/sd/vixen-32/download"


def get(url, path=None, chunk=4096):
    import network
    if not network.LAN().isconnected():
        import net
        net.up()
    if path is None:
        name = url.split("?")[0].rstrip("/").rsplit("/", 1)[-1] or "index.html"
        path = FOLDER + "/" + name
    t = time.ticks_ms()
    r = requests.get(url)
    try:
        if r.status_code != 200:
            raise OSError("HTTP %d" % r.status_code)
        total = int(r.headers.get("Content-Length", 0)) if hasattr(r, "headers") else 0
        buf = bytearray(chunk)
        done = 0
        with open(path, "wb") as f:
            while True:
                n = r.raw.readinto(buf)
                if not n:
                    break
                f.write(buf if n == chunk else buf[:n])
                done += n
                if total:
                    print("\r%s  %d / %d KB" % (path, done // 1024, total // 1024), end="")
                else:
                    print("\r%s  %d KB" % (path, done // 1024), end="")
    finally:
        r.close()
    secs = max(1, time.ticks_diff(time.ticks_ms(), t)) / 1000
    print("\r%s  %d bytes in %.1f s (%.1f KB/s)" % (path, done, secs, done / 1024 / secs))
    return path

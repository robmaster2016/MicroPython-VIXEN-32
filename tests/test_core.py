# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# Interpreter basics and the port's modules. Runs offline, any build.
import sys, gc, os, time, json, re, struct, binascii, vixen

fails = 0


def check(name, cond):
    global fails
    print("ok  " if cond else "FAIL", name)
    if not cond:
        fails += 1


check("platform", sys.platform == "vixen" or len(sys.platform) > 0)
check("implementation", sys.implementation.name == "micropython")
print("   ", sys.version, sys.implementation._machine)

# language
check("bigint", 2 ** 200 // 3 ** 50 == 2238393297946874000179418290327143433)
check("float", abs(sum(1 / (i * i) for i in range(1, 2000)) - 1.6444) < 1e-3)
check("comprehension", {k: v for k, v in zip("abc", range(3))} == {"a": 0, "b": 1, "c": 2})
check("fstring", f"{3.14159:.2f}|{255:#x}" == "3.14|0xff")


def gen(n):
    for i in range(n):
        yield i * i


check("generator", list(gen(5)) == [0, 1, 4, 9, 16])


class A:
    def __init__(self, x):
        self.x = x

    def __add__(self, o):
        return A(self.x + o.x)


check("class", (A(2) + A(3)).x == 5)
try:
    1 / 0
except ZeroDivisionError:
    check("exception", True)

# standard modules
check("json", json.loads(json.dumps({"a": [1, 2.5, None, True]})) == {"a": [1, 2.5, None, True]})
check("re", re.match(r"(\w+)-(\d+)", "VIXEN-32").group(2) == "32")
check("struct", struct.unpack("<HI", struct.pack("<HI", 0xBEEF, 0xC0FFEE)) == (0xBEEF, 0xC0FFEE))
check("binascii", binascii.a2b_base64(binascii.b2a_base64(b"magic")) == b"magic")

# time: the raster-line timebase must advance
t = time.ticks_ms()
time.sleep_ms(120)
dt = time.ticks_diff(time.ticks_ms(), t)
check("ticks_ms advances (%d ms)" % dt, 100 <= dt < 1000)

# memory: the gc heap lives in chip RAM and is large
gc.collect()
free = gc.mem_free()
check("gc heap > 900 KB (%d)" % free, free > 900000)
big = bytearray(512 * 1024)
big[-1] = 42
check("512 KB allocation", big[-1] == 42)
del big

# port modules
for name in ("vixen", "network", "socket", "tls", "cryptolib", "hashlib", "curve25519",
             "sound", "mip", "requests", "net", "sshd", "machine"):
    try:
        __import__(name)
        check("import " + name, True)
    except ImportError as e:
        check("import %s (%s)" % (name, e), False)
for attr in ("tile_scroll", "add_entropy", "seed_file"):
    check("vixen." + attr, hasattr(vixen, attr))

print("TEST OK" if not fails else "TEST FAILED (%d)" % fails)

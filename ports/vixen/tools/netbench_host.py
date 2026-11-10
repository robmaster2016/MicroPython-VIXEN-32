#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
"""netbench, PC side: python3 netbench_host.py <vixen-ip> [port] [size_kb]

Start `import netbench; netbench.run()` on the VIXEN first. Every test is one
TCP connection; the time is measured here on the PC, from the command until
the VIXEN's "OK" line has arrived.
"""
import socket
import sys
import time

HOST = sys.argv[1]
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 5001
SIZE = (int(sys.argv[3]) if len(sys.argv) > 3 else 512) * 1024


def test(op, n, chunk, dest=""):
    s = socket.create_connection((HOST, PORT), timeout=120)
    t = time.monotonic()
    s.sendall(("%s %d %d %s\n" % (op, n, chunk, dest)).encode())
    if op == "RX":
        block = b"\x55" * 65536
        left = n
        while left:
            k = min(left, len(block))
            s.sendall(block[:k])
            left -= k
    got = 0
    tail = b""
    while True:
        d = s.recv(65536)
        if not d:
            break
        if op == "TX" and got < n:
            use = min(len(d), n - got)
            got += use
            d = d[use:]
        tail += d
    dt = time.monotonic() - t
    s.close()
    f = tail.decode().split()
    done, drops, crc, rx, tx = (int(x) for x in f[1:6])
    name = {"RX": "PC -> VIXEN", "TX": "VIXEN -> PC", "SD": "SD write only"}[op]
    print("%-14s %-4s chunk %5d: %7.1f KB/s  (%d B in %.2f s, hw drops %d, crc %d, frames rx %d tx %d)"
          % (name, dest, chunk, done / 1024 / dt, done, dt, drops, crc, rx, tx), flush=True)


print("VIXEN %s:%d, %d KB per test" % (HOST, PORT, SIZE // 1024))
test("RX", SIZE, 4096, "mem")
test("TX", SIZE, 4096)
test("TX", SIZE, 1460)
test("SD", SIZE // 2, 4096)
test("SD", SIZE // 2, 1460)
test("RX", SIZE // 2, 4096, "sd")
test("RX", SIZE // 2, 1460, "sd")

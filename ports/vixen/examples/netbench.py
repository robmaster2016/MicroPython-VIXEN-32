# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# netbench -- where does the transfer time go? (VIXEN side)
#
#   import netbench; netbench.run()          # waits on TCP port 5001
#
# Then on the PC:  python3 netbench_host.py <vixen-ip>
#
# The PC sends one command line per connection and takes the time itself:
# the VIXEN clock (ticks_ms) only advances while it is polled, so a long SD
# write would make it run slow. Commands:
#   RX n chunk dest   receive n bytes; dest "mem" (discard) or "sd" (file)
#   TX n chunk        send n bytes from memory
#   SD n chunk        write n bytes to the SD card, no network involved
# Each answer ends with one line "OK <hw drops> <crc errors> <rx> <tx>".
import network
import socket
import os

PORT = 5001
FILE = "/sd/vixen-32/download/netbench.tmp"


def _stats():
    return network.LAN().status("stats")   # rx frames, tx frames, drops, crc


def _line(c):
    b = b""
    while not b.endswith(b"\n"):
        d = c.recv(1)
        if not d:
            break
        b += d
    return b.decode().split()


def _sd_write(n, chunk):
    buf = bytes(chunk)
    with open(FILE, "wb") as f:
        left = n
        while left > 0:
            k = min(chunk, left)
            f.write(buf if k == chunk else buf[:k])
            left -= k
    return n


def _rx_sd(c, n, chunk):
    # receive and write in blocks of exactly `chunk` bytes
    mv = memoryview(bytearray(chunk))
    left = n
    with open(FILE, "wb") as f:
        while left > 0:
            want = min(chunk, left)
            got = 0
            while got < want:
                k = c.readinto(mv[got:want])
                if not k:
                    return n - left
                got += k
            f.write(mv[:got])
            left -= got
    return n


def _one(c):
    cmd = _line(c)
    if not cmd:
        return
    op, n, chunk = cmd[0], int(cmd[1]), int(cmd[2])
    s0 = _stats()
    done = 0
    if op == "RX" and cmd[3] == "sd":
        done = _rx_sd(c, n, chunk)
    elif op == "RX":
        buf = bytearray(chunk)
        while done < n:
            k = c.readinto(buf)
            if not k:
                break
            done += k
    elif op == "TX":
        buf = bytes(chunk)
        while done < n:
            done += c.write(buf if n - done >= chunk else buf[:n - done])
    elif op == "SD":
        done = _sd_write(n, chunk)
    s1 = _stats()
    try:
        os.remove(FILE)
    except OSError:
        pass
    c.write(("OK %d %d %d %d %d\n" % (done, (s1[2] - s0[2]) & 0xFFFF,
             (s1[3] - s0[3]) & 0xFFFF, s1[0] - s0[0], s1[1] - s0[1])).encode())
    print(op, n, chunk, cmd[3] if len(cmd) > 3 else "", "->", done, "drops",
          (s1[2] - s0[2]) & 0xFFFF)


def run(port=PORT):
    import net
    lan = net.up()
    print("netbench on %s:%d  (Ctrl-C ends)" % (lan.ifconfig()[0], port))
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port))
    s.listen(1)
    try:
        while True:
            c, _ = s.accept()
            try:
                _one(c)
            except OSError as e:
                print("error", e)
            c.close()
    finally:
        s.close()

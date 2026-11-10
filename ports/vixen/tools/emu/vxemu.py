#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
"""Functional VIXEN-32 emulator for testing the MicroPython network build.

Runs firmware.elf on Unicorn (rv32im). Models: BRAM, chip RAM, raster
counter (wall clock), the Ethernet MAC (registers + packet windows) bridged
over UDP to a QEMU user-mode network (DHCP, DNS, NAT to the internet).
Console: uart_tx_strn / uart_rx_chr / uart_rx_any are intercepted, input is
fed through the raw REPL.

usage: vxemu.py firmware.elf script.py [timeout_s]

VX_FWD="tcp::2121-:2121 ..."  forward host ports into the emulated board (servers)
VX_PROF="sym,..."   profile calls of these C functions: instructions fetched
                   from BRAM / chip RAM, loads and stores per region, and a
                   cycle estimate (model constants VX_PROF_W/_R/_I, see below).
VX_STAY=1          after the script, switch to the friendly REPL and keep
                   running until the timeout (for background servers on dupterm)
VX_SCREEN=1        model the display (screen.py): the console draws into the tile
                   map, LVGL into the framebuffer, the blitter runs FILL/COPY/LINE.
                   Needs VX_FONT=<core source>/rtl/font_rom.hex (the ROM font).
VX_SHOT="a.png,b.png"  with VX_SCREEN: each write to 0x3A1FFFF0
                   (machine.mem32[0x3A1FFFF0] = 0) saves the next screenshot
VX_SHOT_END=file   with VX_SCREEN: save a screenshot when the script is done
VX_TYPE=1          type the script into the normal REPL line by line (as a
                   user would) instead of sending it through the raw REPL
VX_SETUP=file      with VX_TYPE: run this script through the raw REPL first
                   (e.g. sd_ramfs.py + files to put on the RAM /sd), then
                   switch to the normal REPL and type the script
VX_SOUNDLOG=file   log the SID (0x160) and PCM (0x164/0x168) register writes
                   as "time_s reg value" lines, and dump the sound module's
                   chip-RAM window (0x380000..0x7F0000) to file.bin at exit.
"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
from collections import deque

from unicorn import (Uc, UcError, UC_ARCH_RISCV, UC_MODE_RISCV32, UC_HOOK_CODE, UC_HOOK_MEM_INVALID,
                     UC_HOOK_BLOCK, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE)
from unicorn.riscv_const import UC_RISCV_REG_PC, UC_RISCV_REG_RA, UC_RISCV_REG_A0, UC_RISCV_REG_A1, UC_RISCV_REG_SP

ELF, SCRIPT = sys.argv[1], sys.argv[2]
TIMEOUT = float(sys.argv[3]) if len(sys.argv) > 3 else 120.0
# nm of the cross toolchain: VX_NM, else the one next to VIXEN_TOOLCHAIN or on PATH
NM = os.environ.get("VX_NM") or shutil.which("riscv32-esp-elf-nm", path=os.pathsep.join(filter(None, (
    os.environ.get("VIXEN_TOOLCHAIN"),
    os.path.expanduser("~/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20241119/riscv32-esp-elf/bin"),
    os.environ.get("PATH"))))) or "riscv-none-elf-nm"

syms, sizes = {}, {}
for line in subprocess.check_output([NM, "-S", ELF], text=True).splitlines():
    p = line.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)
    elif len(p) == 4:
        syms[p[3]] = int(p[0], 16)
        sizes[p[3]] = int(p[1], 16)

uc = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
uc.mem_map(0x0, 0x80000)
uc.mem_map(0x40000000, 0x1000000)

# ---- load ELF PT_LOAD segments --------------------------------------------
data = open(ELF, "rb").read()
e_phoff, = struct.unpack_from("<I", data, 28)
e_phentsize, e_phnum = struct.unpack_from("<HH", data, 42)
for i in range(e_phnum):
    p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, _, _ = struct.unpack_from("<8I", data, e_phoff + i * e_phentsize)
    if p_type == 1 and p_filesz:
        uc.mem_write(p_vaddr, data[p_offset:p_offset + p_filesz])

# ---- Ethernet over UDP to QEMU's slirp ------------------------------------
UDP_ME, UDP_QEMU = 5555, 5556


def _die_with_parent():
    # Linux: QEMU gets SIGTERM when this process dies, even when it is killed
    # (a stale QEMU would keep the UDP port and swallow the next run's frames).
    try:
        import ctypes
        ctypes.CDLL(None).prctl(1, 15)   # PR_SET_PDEATHSIG, SIGTERM
    except Exception:
        pass

qemu = subprocess.Popen([
    "qemu-system-x86_64", "-M", "none", "-nodefaults", "-display", "none",
    "-netdev", "user,id=u,net=10.0.2.0/24"
               + "".join(",hostfwd=" + f for f in os.environ.get("VX_FWD", "").split() if f),
    "-netdev", f"dgram,id=d,remote.type=inet,remote.host=127.0.0.1,remote.port={UDP_ME},"
               f"local.type=inet,local.host=127.0.0.1,local.port={UDP_QEMU}",
    "-netdev", "hubport,id=h1,hubid=0,netdev=u",
    "-netdev", "hubport,id=h2,hubid=0,netdev=d",
], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, preexec_fn=_die_with_parent)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", UDP_ME))
sock.setblocking(False)

rxq = deque()
eth = {"ctrl": 0, "mac_lo": 0x5849_5602 | (0x56 << 8) * 0, "mac_hi": 0x4E45, "drops": 0,
       "tx": bytearray(2048), "rx_frames": 0, "tx_frames": 0, "crcerr": 0}
eth["mac_lo"] = 0x58495602 & 0  # set below
mac = bytes([0x02, 0x56, 0x49, 0x58, 0x45, 0x4E])
eth["mac_lo"] = int.from_bytes(mac[0:4], "little")
eth["mac_hi"] = int.from_bytes(mac[4:6], "little")


def eth_pump():
    while True:
        try:
            f = sock.recv(2048)
        except BlockingIOError:
            return
        if not (eth["ctrl"] & 1):
            continue
        dst = f[0:6]
        mymac = eth["mac_lo"].to_bytes(4, "little") + eth["mac_hi"].to_bytes(2, "little")
        if dst != mymac and dst != b"\xff" * 6:
            continue        # the hardware filter: unicast to us + broadcast
        if os.environ.get("VX_LATE") and not (eth["ctrl"] & 8):
            eth["crcerr"] += 1   # sampled on the wrong RMII edge: garbage
            continue
        if len(rxq) >= 4:
            eth["drops"] += 1
            continue
        rxq.append(f)


T0 = time.monotonic()
other = {}


def mmio_read(uc, off, size, user):
    if off == 0x004:
        return int((time.monotonic() - T0) * 7.8125e6) % 500
    if off == 0x00C:
        return int((time.monotonic() - T0) * 1e6 / 64) % 312
    if off == 0x264:
        eth_pump()
        return 1 | (2 if rxq else 0)
    if off == 0x268:
        return len(rxq[0]) if rxq else 0
    if off == 0x274:
        return eth["mac_lo"]
    if off == 0x278:
        return eth["mac_hi"]
    if off == 0x27C:
        return eth["drops"] & 0xFFFF
    if off == 0x288:
        return eth["crcerr"] & 0xFFFF
    if off in (0x280, 0x284):
        return 0
    if off == 0x28C:
        return 0x0022_0000
    if 0x100000 <= off < 0x100800:
        if size != 4:
            raise RuntimeError(f"non-word RX window read size {size}")
        o = off - 0x100000
        f = rxq[0] if rxq else b""
        return int.from_bytes(f[o:o + 4].ljust(4, b"\0"), "little")
    return other.get(off, 0)


SOUNDLOG = os.environ.get("VX_SOUNDLOG")
slog = open(SOUNDLOG, "w") if SOUNDLOG else None


SCREEN = None
if os.environ.get("VX_SCREEN"):
    from screen import Screen, SHOT
    SCREEN = Screen(uc, other, os.environ["VX_FONT"])
    SHOTS = [f for f in os.environ.get("VX_SHOT", "").split(",") if f]


def take_shot(path):
    print(f"\n[emu] screenshot {SCREEN.render(path)}")


def mmio_write(uc, off, size, value, user):
    if SCREEN:
        if off == SHOT:
            if SHOTS:
                take_shot(SHOTS.pop(0))
            return
        SCREEN.write(off, value)
    if slog and off in (0x160, 0x164, 0x168):
        slog.write(f"{time.monotonic() - T0:.6f} {off:03x} {value:08x}\n")
    if off == 0x260:
        eth["ctrl"] = value
    elif off == 0x26C:
        if rxq:
            rxq.popleft()
            eth["rx_frames"] += 1
    elif off == 0x270:
        frame = bytes(eth["tx"][:value])
        if eth["ctrl"] & 1:
            sock.sendto(frame, ("127.0.0.1", UDP_QEMU))
            eth["tx_frames"] += 1
    elif off == 0x274:
        eth["mac_lo"] = value
    elif off == 0x278:
        eth["mac_hi"] = value & 0xFFFF
    elif 0x100800 <= off < 0x101000:
        if size != 4:
            raise RuntimeError(f"non-word TX window write size {size} at {off:#x}")
        o = off - 0x100800
        eth["tx"][o:o + 4] = value.to_bytes(4, "little")
    else:
        other[off] = value


uc.mmio_map(0x3A000000, 0x200000, mmio_read, None, mmio_write, None)

# ---- console --------------------------------------------------------------
STAY = os.environ.get("VX_STAY")
inp = deque()
out = bytearray()
script = open(SCRIPT, "rb").read()
state = {"phase": 0, "done": False}


def ret(uc, val=None):
    if val is not None:
        uc.reg_write(UC_RISCV_REG_A0, val & 0xFFFFFFFF)
    uc.reg_write(UC_RISCV_REG_PC, uc.reg_read(UC_RISCV_REG_RA))


def on_tx(uc, addr, size, user):
    buf = uc.reg_read(UC_RISCV_REG_A0)
    n = uc.reg_read(UC_RISCV_REG_A1)
    s = bytes(uc.mem_read(buf, n))
    out.extend(s)
    if state["phase"] >= 2:
        sys.stdout.write(s.decode("latin-1").replace("\x04", ""))
        sys.stdout.flush()
    if not SCREEN:
        ret(uc)          # with VX_SCREEN the console's own code draws the text


TYPE = os.environ.get("VX_TYPE")
SETUP = open(os.environ["VX_SETUP"], "rb").read() if os.environ.get("VX_SETUP") else None
typed = script.replace(b"\r\n", b"\n").split(b"\n")
while typed and not typed[-1].strip():
    typed.pop()


def feed():
    if TYPE:
        if SETUP and state["phase"] == 0 and b">>> " in out:
            inp.extend(b"\x01")
            state["phase"] = 5
        elif state["phase"] == 5 and out.endswith(b">"):
            inp.extend(SETUP + b"\x04")
            out.clear()
            state["phase"] = 6
        elif state["phase"] == 6 and out.count(b"\x04") >= 2:
            if b"Traceback" in out:
                sys.stdout.write(out.decode("latin-1"))
            inp.extend(b"\x02")
            out.clear()
            state["phase"] = 7
        elif state["phase"] in (0, 7) and b">>> " in out:
            state["phase"] = 2
        if state["phase"] == 2 and (out.endswith(b">>> ") or out.endswith(b"... ")) and not inp:
            if typed:
                inp.extend(typed.pop(0) + b"\r")
                out.clear()
            elif out.endswith(b">>> "):
                state["done"] = True
                if SCREEN and os.environ.get("VX_SHOT_END"):
                    take_shot(os.environ["VX_SHOT_END"])
                uc.emu_stop()
        return
    if state["phase"] == 0 and b">>> " in out:
        inp.extend(b"\x01")            # raw REPL
        state["phase"] = 1
    elif state["phase"] == 1 and out.endswith(b">"):
        inp.extend(script + b"\x04")
        state["phase"] = 2
        out.clear()
    elif state["phase"] == 2 and out.count(b"\x04") >= 2:
        state["done"] = True
        if SCREEN and os.environ.get("VX_SHOT_END"):
            take_shot(os.environ["VX_SHOT_END"])
        if STAY:
            inp.extend(b"\x02")        # leave the raw REPL, keep running
            state["phase"] = 3
        else:
            uc.emu_stop()


# Once the script runs and no scripted input is pending, let the firmware's
# own uart_rx_any/uart_rx_chr run: they call uart_poll(), which pumps the
# Ethernet stack -- a script waiting in input() (e.g. with os.dupterm on a
# socket) must keep the network alive, as on the hardware.
def on_rx_any(uc, addr, size, user):
    feed()
    if inp or state["phase"] < 2:
        ret(uc, 1 if inp else 0)


def on_rx_chr(uc, addr, size, user):
    feed()
    if inp or state["phase"] < 2:
        ret(uc, inp.popleft() if inp else 0xFFFFFFFF)


for name, cb in (("uart_tx_strn", on_tx), ("uart_rx_any", on_rx_any), ("uart_rx_chr", on_rx_chr)):
    a = syms[name]
    uc.hook_add(UC_HOOK_CODE, cb, begin=a, end=a)


def on_bad(uc, access, addr, size, value, user):
    print(f"\n[emu] invalid memory access {access} at {addr:#x} pc={uc.reg_read(UC_RISCV_REG_PC):#x}")
    return False


uc.hook_add(UC_HOOK_MEM_INVALID, on_bad)

# ---- profiler (VX_PROF) ---------------------------------------------------
# Cycle model, calibrated against the hardware: a BRAM instruction 1 cycle,
# a chip-RAM instruction PROF_I (cache hit), a chip-RAM load PROF_R (cache hit,
# 3 per the chipset reference), a chip-RAM store PROF_W (write-through to the
# SDRAM, ~9.3 MB/s measured).
PROF_W = float(os.environ.get("VX_PROF_W", "40"))
PROF_R = float(os.environ.get("VX_PROF_R", "3"))
PROF_I = float(os.environ.get("VX_PROF_I", "1.75"))
prof = {"depth": 0, "hooks": []}


def _blk(uc, addr, size, user):
    prof["ic" if addr >= 0x40000000 else "ib"] += size // 4


def _rd(uc, access, addr, size, value, user):
    prof["rc" if addr >= 0x40000000 else "rb"] += 1


def _wr(uc, access, addr, size, value, user):
    prof["wc" if addr >= 0x40000000 else "wb"] += 1


def _prof_enter(uc, addr, name):
    if prof["depth"] == 0:
        prof.update(ib=0, ic=0, rb=0, rc=0, wb=0, wc=0, name=name, sp0=uc.reg_read(UC_RISCV_REG_SP), spmin=1 << 32)
        prof["hooks"] = [uc.hook_add(UC_HOOK_BLOCK, _blk), uc.hook_add(UC_HOOK_MEM_READ, _rd),
                         uc.hook_add(UC_HOOK_MEM_WRITE, _wr)]
        ra = uc.reg_read(UC_RISCV_REG_RA)
        prof["hooks"].append(uc.hook_add(UC_HOOK_CODE, _prof_exit, begin=ra, end=ra))
    prof["depth"] += 1


def _prof_exit(uc, addr, size, user):
    prof["depth"] = 0
    for h in prof["hooks"]:
        uc.hook_del(h)
    p = prof
    cyc = p["ib"] + PROF_I * p["ic"] + PROF_R * p["rc"] + PROF_W * p["wc"]
    print(f"\n[prof] {p['name']}: instr bram={p['ib']} chip={p['ic']}  load bram={p['rb']} chip={p['rc']}"
          f"  store bram={p['wb']} chip={p['wc']}  ~{cyc / 1e8:.3f} s @100MHz")


for _n in filter(None, os.environ.get("VX_PROF", "").split(",")):
    uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, u, n=_n: _prof_enter(uc, a, n), begin=syms[_n], end=syms[_n])

t_start = time.monotonic()
try:
    uc.emu_start(syms["_start"], 0xFFFFFFFF, timeout=int(TIMEOUT * 1e6))
except UcError as e:
    print(f"\n[emu] {e} pc={uc.reg_read(UC_RISCV_REG_PC):#x} sp={uc.reg_read(UC_RISCV_REG_SP):#x}")
finally:
    qemu.terminate()
el = time.monotonic() - t_start
print(f"\n[emu] {'done' if state['done'] else 'TIMEOUT/STOP'} after {el:.1f}s; "
      f"eth rx={eth['rx_frames']} tx={eth['tx_frames']} drops={eth['drops']}")
ss, es = syms["_sstack"], syms["_estack"]
stk = bytes(uc.mem_read(ss, es - ss))
free = 0
while free + 4 <= len(stk) and stk[free:free + 4] == b"\xbb" * 4:
    free += 4
print(f"[emu] C stack used {len(stk) - free} of {len(stk)} bytes")
if "c25519_stack" in syms:   # BRAM, zero in the emulator until first use; 16-byte canary
    cs = bytes(uc.mem_read(syms["c25519_stack"] + 16, sizes["c25519_stack"] - 16))
    unused = len(cs) - len(cs.lstrip(b"\0"))
    print(f"[emu] curve25519 BRAM stack used {len(cs) - unused} of {len(cs)} bytes")
if slog:
    slog.close()
    with open(SOUNDLOG + ".bin", "wb") as fb:
        fb.write(bytes(uc.mem_read(0x40380000, 0x470000)))
if not state["done"]:
    sys.stdout.write(out[-2000:].decode("latin-1"))

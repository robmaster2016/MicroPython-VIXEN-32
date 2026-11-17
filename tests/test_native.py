# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# Native code: @micropython.native, @micropython.viper and @micropython.asm_rv32.
# The VIXEN core has no C extension; the port builds the RV32 emitter with
# MICROPY_EMIT_RV32_RVC = 0 (patches/micropython-rv32-norvc.patch). Run the
# emulator with VX_RVC=1 to stop at any compressed instruction (run_tests.sh
# does, see "# vx: rvc").
# vx: rvc
import micropython


@micropython.native
def fib(n):
    a, b = 0, 1
    for _ in range(n):
        a, b = b, a + b
    return a


@micropython.native
def calls(xs):
    out = []
    for x in xs:
        out.append(str(x * 3) + "!")
    try:
        raise ValueError("native")
    except ValueError as e:
        out.append(e.args[0])
    return out


@micropython.viper
def vsum(n: int) -> int:
    s = 0
    i = 0
    while i < n:
        if i & 1:
            s += i * 3
        else:
            s -= i >> 1
        i += 1
    return s


@micropython.viper
def vbuf(b) -> int:
    p8 = ptr8(b)
    p32 = ptr32(b)
    n = int(len(b))
    for i in range(n):
        p8[i] = (i * 7 + 1) & 0xFF
    p32[1] = 0x12345678
    p32[300] = p32[1] + 1           # far offset (no short form)
    acc = 0
    for i in range(n):
        acc += p8[i]
    return acc + p32[300]


@micropython.viper
def vlong(n: int) -> int:
    # a long loop body: branches beyond the short (8/12-bit) reach
    s = 0
    for i in range(n):
        s += i; s ^= 0x55; s += i * 2; s ^= 0xAA; s += i * 3; s ^= 0x33; s += i * 4
        s ^= 0xCC; s += i * 5; s ^= 0x0F; s += i * 6; s ^= 0xF0; s += i * 7; s ^= 0x3C
        s += i; s ^= 0x55; s += i * 2; s ^= 0xAA; s += i * 3; s ^= 0x33; s += i * 4
        s ^= 0xCC; s += i * 5; s ^= 0x0F; s += i * 6; s ^= 0xF0; s += i * 7; s ^= 0x3C
        s += i; s ^= 0x55; s += i * 2; s ^= 0xAA; s += i * 3; s ^= 0x33; s += i * 4
        s ^= 0xCC; s += i * 5; s ^= 0x0F; s += i * 6; s ^= 0xF0; s += i * 7; s ^= 0x3C
        s += i; s ^= 0x55; s += i * 2; s ^= 0xAA; s += i * 3; s ^= 0x33; s += i * 4
        s ^= 0xCC; s += i * 5; s ^= 0x0F; s += i * 6; s ^= 0xF0; s += i * 7; s ^= 0x3C
        s &= 0xFFFFFF
    return s


def py_long(n):
    s = 0
    for i in range(n):
        for _ in range(4):
            s += i; s ^= 0x55; s += i * 2; s ^= 0xAA; s += i * 3; s ^= 0x33; s += i * 4
            s ^= 0xCC; s += i * 5; s ^= 0x0F; s += i * 6; s ^= 0xF0; s += i * 7; s ^= 0x3C
        s &= 0xFFFFFF
    return s


@micropython.asm_rv32
def asm_add(a0, a1):
    add(a0, a0, a1)
    slli(a0, a0, 1)


assert fib(30) == 832040, fib(30)
assert calls([1, 12, 345]) == ["3!", "36!", "1035!", "native"]
assert vsum(1000) == sum(i * 3 if i & 1 else -(i >> 1) for i in range(1000)), vsum(1000)
b = bytearray(1600)
got = vbuf(b)
# p32 writes changed bytes 4..7 and 1200..1203 after the byte pass
exp = sum(b) + 0x12345679
assert got == exp, (got, exp)
assert vlong(300) == py_long(300), (vlong(300), py_long(300))
assert asm_add(20, 22) == 84

# a loop body of > 4 KB of code: the conditional branch back needs the long
# form (beq over auipc + jalr), whose skip distance depends on opcode sizes
body = "".join("        s += i * %d; s ^= %d\n" % (k % 7 + 1, k * 37 & 0xFF) for k in range(160))
src = ("@micropython.viper\ndef huge(n: int) -> int:\n    s = 0\n    i = 0\n"
       "    while i < n:\n" + body + "        s &= 0xFFFFFF\n        i += 1\n    return s\n")
g = {"micropython": micropython}
exec(src, g)
src_py = src.replace("@micropython.viper\n", "").replace("(n: int) -> int", "(n)")
gp = {}
exec(src_py, gp)
assert g["huge"](50) == gp["huge"](50), (g["huge"](50), gp["huge"](50))
print("native ok")
print("TEST OK")

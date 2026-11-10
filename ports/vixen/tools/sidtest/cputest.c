/* SPDX-License-Identifier: MIT
 * Copyright (c) 2025-2026 Robert S. (aka Robmaster) */
/* PC test of vx6502.h against Klaus Dormann's 6502_functional_test.bin
 * (load at 0, start $0400, success = PC stuck at $3469). */
#include <stdio.h>
#include <stdlib.h>
#define VX6502_REAL_BRK
static unsigned char mem[65536];
static unsigned char vx_io_rd(unsigned short a) { return mem[a]; }
static void vx_io_wr(unsigned short a, unsigned char v) { mem[a] = v; }
#include "../../vx6502.h"
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(mem, 1, 65536, f) != 65536) { puts("load"); return 2; }
    vx6502_t c = { mem, 0x0400, 0, 0, 0, 0xFF, VX_U };
    unsigned long long total = 0;
    uint16_t last = 0;
    for (;;) {
        uint32_t used;
        last = c.pc;
        int r = vx6502_run(&c, 0x3469, 1, &used);   /* one instruction at a time */
        total += used;
        if (r == VX_RUN_TRAP) { printf("PASS after %llu cycles\n", total); return 0; }
        if (r == VX_RUN_HALT) { printf("HALT at %04X\n", c.pc); return 1; }
        if (c.pc == last) { printf("FAIL: stuck at %04X (a=%02X x=%02X y=%02X p=%02X)\n", c.pc, c.a, c.x, c.y, c.p); return 1; }
    }
}

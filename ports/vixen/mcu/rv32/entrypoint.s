/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2023 Alessandro Gatti
 * Copyright (c) 2025 Leif A. Pettersen (VIXEN-32 adaptation)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* VIXEN-32 entry: runs from the on-chip BRAM. -nostartfiles means newlib's
 * crt0 is skipped, so this _start is the first code that runs. */

        .section .start
        .option  norvc           /* Do not emit compressed instructions. */
        .type    _start, @function
        .global  _start

_start:
        .cfi_startproc

        .option push
        .option norelax
        la   gp, _global_pointer /* Load global pointer register. */
        .option pop

        /* NO 'csrw satp': Murax (rv32im_zicsr) has no MMU, satp is
         * unimplemented, and writing an unimplemented CSR raises an
         * illegal-instruction trap -> mtvec -> the KERNAL's handler, which
         * drops back to the VIXEN-32 splash. The proven devkit crt0 never
         * touches satp; satp resets to 0 (bare M-mode) on its own. */

        /* Early heartbeat: paint one cell (an 'X', fg=14 on bg=6) at tile
         * (0,0) of the tile map BEFORE touching chip-RAM. If this shows and
         * the banner does not, the fault is in the chip-RAM stack/BSS below.
         * Tile map cell = ch | fg<<8 | bg<<16 = 0x58 | 0x0E00 | 0x060000. */
        li   t0, 0x3A038A00      /* VIH_TILE_MAP0_BASE */
        li   t1, 0x00060E58      /* 'X' white-on-blue */
        sw   t1, 0(t0)

        /* Invalidate the CPU cache over the chip-RAM window: the .rvk loader
         * wrote chip RAM (the LVGL build runs code from there, .chiptext), and
         * the cache does not watch the loader. ~10 us; harmless otherwise. */
        li   t0, 0x3A0001E4      /* VIH_CACHE_INVAL */
        li   t1, 1
        sw   t1, 0(t0)

        /* Fill the stack with a canary value (chip-RAM). */

        li   t0, 0xBBBBBBBB      /* Canary. */
        la   t1, _sstack         /* Stack start. */
        la   t2, _estack         /* Stack end. */
1:
        sw   t0, (t1)            /* Write canary. */
        addi t1, t1, 4           /* Next word. */
        bltu t1, t2, 1b          /* Until the stack is filled. */

        la   sp, _estack         /* Stack pointer at the top of the stack. */

        /* Clear BSS. Two ranges: gp-relative .sbss in BRAM, and the large
         * .bss in chip-RAM (each bracketed by its own _s/_e symbols). */

        la   t1, _sbss_bram      /* BRAM .sbss start. */
        la   t2, _ebss_bram      /* BRAM .sbss end. */
1:
        sw   zero, (t1)          /* Clear word. */
        addi t1, t1, 4           /* Next word. */
        bltu t1, t2, 1b          /* Until BRAM .sbss is cleared. */

        la   t1, _sbss           /* chip-RAM .bss start. */
        la   t2, _ebss           /* chip-RAM .bss end. */
1:
        sw   zero, (t1)          /* Clear word. */
        addi t1, t1, 4           /* Next word. */
        bltu t1, t2, 1b          /* Until chip-RAM .bss is cleared. */

        /* Jump into C code. */

        la   t0, _entry_point
        tail _entry_point        /* Jump to the C entry point. */

        .cfi_endproc
        .end

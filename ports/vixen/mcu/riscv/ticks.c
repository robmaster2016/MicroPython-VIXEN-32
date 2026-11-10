/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2025 Ibrahim Abdelkader <iabdalkader@openmv.io>
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
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdint.h>

#include "vih_regs.h"

// VIXEN-32 timebase: the VIH raster line counter.
//
// The Murax core only implements mstatus/mie/mcause/mip -- there is no
// mcycle/mtime CSR, so rdcycle/rdtime cannot be used (they read as a constant
// and time stood still: no cursor blink, time.sleep() never returned).
//
// VIH_RASTER_VPOS counts the PAL beam line 0..311; one line is exactly 64 us
// (500 pixels at the 7.8125 MHz pixel clock), one frame 312 lines = 19.968 ms.
// ticks_poll() accumulates the lines elapsed since the previous call. It must
// therefore run at least once per frame (< ~20 ms) or whole frames are lost:
// ticks_ms/ticks_us call it themselves, and MICROPY_VM_HOOK_POLL calls it
// while Python code runs (mpconfigport.h). Long-running C code (e.g. a big
// gc collect) can still make the clock lag slightly -- it never runs fast.
#define TICKS_LINES_PER_FRAME   312u
#define TICKS_US_PER_LINE       64u

static uint32_t ticks_last_vpos;
static uint64_t ticks_lines;

void ticks_poll(void) {
    uint32_t vpos = VIH_RASTER_VPOS;
    if (vpos >= TICKS_LINES_PER_FRAME) {
        return;   // out of range (should not happen): skip this sample
    }
    uint32_t delta = vpos + TICKS_LINES_PER_FRAME - ticks_last_vpos;
    if (delta >= TICKS_LINES_PER_FRAME) {
        delta -= TICKS_LINES_PER_FRAME;
    }
    ticks_last_vpos = vpos;
    ticks_lines += delta;
}

uintptr_t ticks_ms(void) {
    ticks_poll();
    return (uintptr_t)(ticks_lines * TICKS_US_PER_LINE / 1000u);
}

uintptr_t ticks_us(void) {
    ticks_poll();
    return (uintptr_t)(ticks_lines * TICKS_US_PER_LINE);
}

// Raster lines elapsed since start (64 us each), for code that times against
// the beam itself (vx_lvgl.c waits for the GFX pointer latch with it).
uint64_t ticks_raster_lines(void) {
    ticks_poll();
    return ticks_lines;
}

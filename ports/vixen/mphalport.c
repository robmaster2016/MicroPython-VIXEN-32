/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2024 Damien P. George
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

#include "py/mphal.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "extmod/misc.h"
#include "uart.h"
#include "vih_regs.h"

// VIXEN-32 console is the VIH text layer (see uart.c).
#define USE_UART (1)

uintptr_t ticks_ms(void);
uintptr_t ticks_us(void);

// os.dupterm(stream): a second console (e.g. a network REPL) next to the
// screen/keyboard. Its input is polled here, alongside the keyboard; its
// output gets everything the screen gets. Ctrl-C from it only interrupts
// while the REPL or input() is reading -- a running script does not poll it.
uintptr_t mp_hal_stdio_poll(uintptr_t poll_flags) {
    uintptr_t ret = 0;
    #if USE_UART
    if ((poll_flags & MP_STREAM_POLL_RD) && uart_rx_any()) {
        ret |= MP_STREAM_POLL_RD;
    }
    #endif
    #if MICROPY_PY_OS_DUPTERM
    ret |= mp_os_dupterm_poll(poll_flags);
    #endif
    return ret;
}

int mp_hal_stdin_rx_chr(void) {
    for (;;) {
        #if USE_UART
        int c = uart_rx_chr();
        if (c >= 0) {
            return c;
        }
        #endif
        #if MICROPY_PY_OS_DUPTERM
        int d = mp_os_dupterm_rx_chr();
        if (d >= 0) {
            return d;
        }
        #endif
        // lets Ctrl-C interrupt a script blocked in input()
        mp_handle_pending(true);
    }
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    #if USE_UART
    uart_tx_strn(str, len);
    #endif
    #if MICROPY_PY_OS_DUPTERM
    mp_os_dupterm_tx_strn(str, len);
    #endif
    return len;
}

mp_uint_t mp_hal_ticks_ms(void) {
    return ticks_ms();
}

mp_uint_t mp_hal_ticks_us(void) {
    return ticks_us();
}

void mp_hal_delay_ms(mp_uint_t ms) {
    if (ms) {
        mp_uint_t start = mp_hal_ticks_ms();
        while (mp_hal_ticks_ms() - start < ms) {
            // keep the keyboard serviced so Ctrl-C can interrupt time.sleep()
            uart_poll();
            mp_handle_pending(true);
        }
    } else {
        mp_handle_pending(true);
    }
}

void mp_hal_delay_us(mp_uint_t us) {
    mp_uint_t start = mp_hal_ticks_us();
    while (mp_hal_ticks_us() - start < us) {
    }
}

mp_uint_t mp_hal_ticks_cpu(void) {
    return 0;
}

// mp_hal_get_random() and vixen_random_u32(): vixen_random.c

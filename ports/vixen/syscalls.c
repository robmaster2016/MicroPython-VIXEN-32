/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
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

/* Minimal newlib syscall stubs for bare-metal VIXEN-32.
 *
 * We link libc (-nostartfiles, not -nostdlib), so newlib expects the usual
 * syscall entry points. These route stdio to the console and back a tiny
 * brk-heap for malloc. TODO(VIXEN): _read should pull from the VIH keyboard
 * FIFO once the console input path (phase 2) is wired.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <reent.h>

#include "uart.h"

/* From mcu/rv32/vixen.ld */
extern char _sheap[];
extern char _eheap[];

static char *brk_ptr = NULL;

/* newlib reaches errno (and lgamma's signgam) through __getreent(). Some
 * toolchains (e.g. the Espressif riscv32-esp-elf newlib) only ship a weak stub
 * returning NULL, so every errno write would land at address 0x0 -- the reset
 * jump / trap vector page. Provide one real, single-threaded reent struct. */
static struct _reent vixen_reent;

struct _reent *__getreent(void) {
    return &vixen_reent;
}

void *_sbrk(ptrdiff_t incr) {
    if (brk_ptr == NULL) {
        brk_ptr = _sheap;
    }
    char *prev = brk_ptr;
    if (prev + incr > _eheap) {
        errno = ENOMEM;
        return (void *)-1;
    }
    brk_ptr += incr;
    return (void *)prev;
}

int _write(int fd, const char *buf, int count) {
    if (fd == 1 || fd == 2) {
        uart_tx_strn(buf, (size_t)count);
        return count;
    }
    return -1;
}

int _read(int fd, char *buf, int count) {
    (void)fd;
    (void)buf;
    (void)count;
    return 0;
}

int _fstat(int fd, struct stat *st) {
    (void)fd;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int fd) {
    return (fd == 1 || fd == 2) ? 1 : 0;
}

int _lseek(int fd, int offset, int whence) {
    (void)fd;
    (void)offset;
    (void)whence;
    return -1;
}

int _kill(int pid, int sig) {
    (void)pid;
    (void)sig;
    return 0;
}

int _getpid(void) {
    return 1;
}

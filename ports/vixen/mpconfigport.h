/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2014-2024 Damien P. George
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

// options to control how MicroPython is built

#define MICROPY_CONFIG_ROM_LEVEL    (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)

// The whole interpreter lives in the 16 MB cached chip-RAM window (see
// mcu/rv32/vixen.ld), so there is no BRAM size budget to trim against. Build
// with the full feature set: compiler, GC, REPL, float, bignum, VFS/file import.
#define MICROPY_ENABLE_COMPILER     (1)
#define MICROPY_ENABLE_GC           (1)
#define MICROPY_HELPER_REPL         (1)
#define MICROPY_KBD_EXCEPTION       (1)
#define MICROPY_PY_SYS              (1)
#define MICROPY_FLOAT_IMPL          (MICROPY_FLOAT_IMPL_FLOAT)

#if defined(__ARM_ARCH_ISA_ARM)
#define MICROPY_EMIT_ARM            (1)
#define MICROPY_EMIT_INLINE_THUMB   (1)
#elif defined(__ARM_ARCH_ISA_THUMB)
#if !defined(QEMU_SOC_NRF51)
#define MICROPY_EMIT_THUMB          (1)
#define MICROPY_EMIT_INLINE_THUMB   (1)
#endif
#define MICROPY_MAKE_POINTER_CALLABLE(p) ((void *)((mp_uint_t)(p) | 1))
#elif defined(__riscv)
#if (__riscv_xlen == 32)
// VIXEN-32 is rv32im_zicsr. Native codegen emits plain rv32im (no C/Zba), which
// the core fetches directly from the chip-RAM window.
#define MICROPY_EMIT_RV32           (1)
#define MICROPY_EMIT_INLINE_RV32    (1)
#elif (__riscv_xlen == 64)
#define MICROPY_PERSISTENT_CODE_LOAD_NATIVE (1)
#else
#error "Unsupported RISC-V platform!"
#endif
#elif defined(__powerpc__) && defined(__powerpc64__)
#define MICROPY_MAKE_POINTER_CALLABLE(p) ((void *)((mp_uint_t)(p) | 1))
#endif

#define MICROPY_MALLOC_USES_ALLOCATED_SIZE (1)
#define MICROPY_PERSISTENT_CODE_LOAD (1)
#define MICROPY_ENABLE_EMERGENCY_EXCEPTION_BUF (1)
#define MICROPY_LONGINT_IMPL        (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_WARNINGS            (1)
#define MICROPY_PY_SYS_PLATFORM     "vixen"
#define MICROPY_PY_ASYNCIO          (0)
#define MICROPY_PY_MACHINE          (1)
#define MICROPY_PY_MACHINE_INCLUDEFILE "vixen_modmachine.c"
#define MICROPY_PY_MACHINE_RESET    (1)
#define MICROPY_PY_MACHINE_PIN_BASE (0)
// File import / VFS. The chip-RAM budget lets the full VFS framework in: the
// file reader (mp_lexer_new_from_file) drives frozen modules and imports from
// a mounted filesystem. The VIXEN-32 ROM filesystem is wired via vfs_rom_ioctl.c
// (MICROPY_VFS_ROM / MICROPY_VFS_ROM_IOCTL); the SD card and the F12 disk images
// are mounted at /sd, /a, /b through the kernel's disk services (vixen_disk.c).
#define MICROPY_VFS                 (1)
#define MICROPY_READER_VFS          (1)
#define MICROPY_VFS_ROM             (1)
#define MICROPY_VFS_ROM_IOCTL       (1)
#define MICROPY_PY_BTREE            (0)

// Network (make NET=1, the default): lwIP on the core's Ethernet MAC
// (vixen_eth.c) gives network.LAN, socket, DHCP and DNS; mbedtls gives ssl,
// so mip and requests can fetch over HTTPS. modlwip brackets its lwIP calls
// with ENTER/EXIT; vixen_eth_poll() stays out while that lock is held. Its
// blocking loops pump the stack (and the keyboard, for Ctrl-C) via uart_poll.
#if MICROPY_PY_LWIP
#define MICROPY_PY_NETWORK          (1)
#define MICROPY_PY_SOCKET           (1)
#define MICROPY_PY_NETWORK_HOSTNAME_DEFAULT "vixen32"
extern const struct _mp_obj_type_t network_lan_type;
#define MICROPY_PORT_NETWORK_INTERFACES \
    { MP_ROM_QSTR(MP_QSTR_LAN), MP_ROM_PTR(&network_lan_type) },
extern volatile uint32_t vixen_lwip_lock;
#define MICROPY_PY_LWIP_ENTER       vixen_lwip_lock++;
#define MICROPY_PY_LWIP_REENTER     vixen_lwip_lock++;
#define MICROPY_PY_LWIP_EXIT        vixen_lwip_lock--;
#define MICROPY_PY_LWIP_POLL_HOOK   uart_poll();
#endif
// os.urandom (vixen_random.c), os.dupterm (a second console, e.g. over the
// network; wired into mphalport.c) and AES-CTR in cryptolib -- building
// blocks for an SSH server written in Python.
#define MICROPY_PY_OS_URANDOM       (1)
#define MICROPY_PY_OS_DUPTERM       (1)
#define MICROPY_PY_CRYPTOLIB_CTR    (1)
#define MICROPY_PY_CRYPTOLIB_CONSTS (1)

#if MICROPY_SSL_MBEDTLS
// mbedtls allocates from the gc heap through these (MBEDTLS_PLATFORM_STD_CALLOC).
#define MICROPY_TRACKED_ALLOC       (1)
#endif

// type definitions for the specific machine

#if (defined(__riscv) && (__riscv_xlen == 64)) || (defined(__powerpc__) && defined(__powerpc64__))
#define MP_SSIZE_MAX (0x7fffffffffffffff)
#else
#define MP_SSIZE_MAX (0x7fffffff)
#endif

typedef long mp_off_t;

// We need to provide a declaration/definition of alloca()
#include <alloca.h>

// We need an implementation of the log2 function which is not a macro.
#define MP_NEED_LOG2 (1)

#define MP_STATE_PORT MP_STATE_VM

// Poll the console from the VM while bytecode runs: uart_poll() samples the
// raster-line timebase (mcu/riscv/ticks.c, needs a sample at least once per
// 20 ms frame), drains the keyboard FIFO (type-ahead, key repeat) and turns
// Ctrl-C into a KeyboardInterrupt.
void uart_poll(void);
// The recursion check fires this many bytes before the end of the C stack,
// leaving room for the C code that runs below the deepest Python call (the
// exception itself, a print). In the LVGL builds the stack is in BRAM and
// work buffers lie below it (vixen_lvgl.ld).
#define MICROPY_STACK_CHECK_MARGIN (2048)
#define MICROPY_VM_HOOK_COUNT (64)
#define MICROPY_VM_HOOK_INIT static unsigned int vm_hook_divisor = MICROPY_VM_HOOK_COUNT;
#define MICROPY_VM_HOOK_POLL if (--vm_hook_divisor == 0) { \
        vm_hook_divisor = MICROPY_VM_HOOK_COUNT; \
        uart_poll(); \
}
#define MICROPY_VM_HOOK_LOOP MICROPY_VM_HOOK_POLL
#define MICROPY_VM_HOOK_RETURN MICROPY_VM_HOOK_POLL

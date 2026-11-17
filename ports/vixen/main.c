/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2014-2023 Damien P. George
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

#include <stdlib.h>

#include "py/compile.h"
#include "py/runtime.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "shared/runtime/gchelper.h"
#include "shared/runtime/pyexec.h"
#include "py/persistentcode.h"
#include "kernel_api.h"
#include "vih_regs.h"

#if VIXEN_SOUND
void vixen_sound_reset(void);
#endif

#if MICROPY_HEAP_SIZE <= 0
#error MICROPY_HEAP_SIZE must be a positive integer.
#endif

#if MICROPY_STACK_SIZE <= 0
#error MICROPY_STACK_SIZE must be a positive integer.
#endif

/* gc arena lives in the 16 MB chip-RAM window (see vixen.ld, .gcram); the
 * BRAM app region is too small to also hold it. It is NOLOAD, so we must zero
 * it here before gc_init -- unlike BRAM bss, the chip-RAM window is not wiped
 * by the boot path. */
extern char _GC_HEAP_START[];
static void zorro_init(void);

void vixen_disk_automount(void);
void vixen_disk_enter_app_dir(void);
#define VIXEN_APP_DIR "/sd/vixen-32/apps/micropython"
#if VIXEN_LVGL
#include "vx_lvgl.h"
#endif
#if MICROPY_PY_NETWORK
#include "extmod/modnetwork.h"
#endif
#if VIXEN_SQLITE
#include "vixen_sqlite.h"
#endif

void vixen_random_init(void);
void vixen_random_boot_seed(void);

int main(int argc, char **argv) {
    static bool seeded = false;
    mp_cstack_init_with_sp_here(MICROPY_STACK_SIZE);
    vixen_random_init();
    memset(_GC_HEAP_START, 0, MICROPY_HEAP_SIZE);
    gc_init(_GC_HEAP_START, _GC_HEAP_START + MICROPY_HEAP_SIZE);
    zorro_init();
    vih_icache(1u);
    #if VIXEN_SQLITE
    vixen_sqlite_init();
    #endif

    for (;;) {
        mp_init();
        #if MICROPY_PY_NETWORK
        /* The LAN interface and its lwIP state survive a soft reset (the
         * address stays); only the Python-side NIC list starts empty. An
         * exception that escaped between modlwip's ENTER and EXIT must not
         * leave the stack locked out of its poll for good. */
        vixen_lwip_lock = 0;
        mod_network_init();
        #endif
        #if VIXEN_LVGL
        vx_lvgl_mp_reset();
        vx_gfx_reset();
        #endif
        vixen_disk_automount();

        /* Autostart, like other MicroPython boards but from MicroPython's own
         * folder on the card, VIXEN_APP_DIR/boot/: boot.py, then main.py, at
         * power-up and after every soft reset (Ctrl-D). The current directory
         * is VIXEN_APP_DIR (if it exists), so scripts can use relative paths.
         * RUN/STOP or Ctrl-C interrupts a running script and drops to the
         * REPL; a script that calls sys.exit() ends in the REPL as well. */
        vixen_disk_enter_app_dir();
        if (!seeded) {
            /* once per power-up: entropy from earlier sessions, see
             * vixen_random.c (random.seed in VIXEN_APP_DIR) */
            seeded = true;
            vixen_random_boot_seed();
        }
        if (pyexec_mode_kind == PYEXEC_MODE_FRIENDLY_REPL) {
            pyexec_file_if_exists(VIXEN_APP_DIR "/boot/boot.py");
        }
        if (pyexec_mode_kind == PYEXEC_MODE_FRIENDLY_REPL) {
            pyexec_file_if_exists(VIXEN_APP_DIR "/boot/main.py");
        }

        for (;;) {
            if (pyexec_mode_kind == PYEXEC_MODE_RAW_REPL) {
                if (pyexec_raw_repl() != 0) {
                    break;
                }
            } else {
                if (pyexec_friendly_repl() != 0) {
                    break;
                }
            }
        }

        mp_printf(&mp_plat_print, "MPY: soft reboot\n");

        #if VIXEN_SQLITE
        /* before the sweep: SQLite's globals point into the gc heap */
        vixen_sqlite_reset();
        #endif
        /* Runs the finalisers: open sockets are closed here, before the
         * objects lwIP's callbacks point at are gone. */
        gc_sweep_all();
        #if VIXEN_SOUND
        vixen_sound_reset();   /* silence SID and PCM */
        #endif
        #if MICROPY_PY_NETWORK
        mod_network_deinit();
        #endif
        mp_deinit();
    }
}

/* ---- instruction cache (core v0.88+) -----------------------------------
 * 8 KB, for code fetched above the BRAM: everything in .chiptext (the LVGL
 * widgets and binding, extmod, the sound module) and native code on the
 * heap. Measured on the MEGA65: compile() 37 % faster before the compiler
 * moved to BRAM, json 8 %. Code in BRAM runs as before. The kernel switches
 * it off at every .rvk launch; on older cores the write is ignored. RULE: new
 * machine code in RAM -> vih_cache_invalidate() before it runs: the
 * native/viper emitters and native .mpy files commit through here. */
void *vixen_commit_exec(void *buf, size_t len, void *reloc) {
    (void)len;
    if (reloc != NULL) {
        mp_native_relocate(reloc, buf, (uintptr_t)buf);
    }
    vih_cache_invalidate();
    return buf;
}

/* ---- Zorro RAM: room for the gc heap to grow ---------------------------
 * Core v0.89+ has 48 MB more SDRAM at 0x41000000, CPU only (the blitter,
 * video and audio cannot see it). Python objects never go to the hardware --
 * framebuffers, sound and sprites use fixed chip-RAM windows -- so the gc heap
 * may live there. The chip-RAM arena stays the first area; when a collection
 * cannot free enough, gc.c (MICROPY_GC_SPLIT_HEAP_AUTO) asks for a new area
 * that doubles the heap (6 -> 12 -> 24 -> 48 MB ...), and gives an area that
 * is empty after a collection back. A collection only walks the used part of
 * each area, so a small program pays nothing for the 48 MB.
 *
 * The areas come from the kernel's allocator (ksys_alloc_mem, KMEM_ZORRO),
 * so a kernel module that takes Zorro RAM is respected. Kernels without the
 * call (NULL entry, before build 123) and cores without Zorro RAM: no growth,
 * the heap stays MICROPY_HEAP_SIZE. */
static uint32_t zorro_avail;   /* bytes the gc may still ask for */
static struct { void *p; uint32_t size; } zorro_area[32];   /* what it holds */

static void zorro_init(void) {
    zorro_avail = 0;
    if (KERNEL_ENTRY(KENT_SYS_ALLOC_MEM) != NULL && KERNEL_ENTRY(KENT_SYS_ALLOC_FREE) != NULL
        && vih_zorro_present()) {
        zorro_avail = ZORRO_RAM_SIZE;
    }
}

size_t gc_get_max_new_split(void) {
    return zorro_avail;
}

void *vixen_zorro_alloc(size_t size) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(zorro_area); i++) {
        if (zorro_area[i].p == NULL) {
            if (size > zorro_avail) {
                return NULL;
            }
            void *p = ksys_alloc_mem(KMEM_ZORRO, 0, size, 16);
            if (p == NULL) {
                zorro_avail = 0;    /* the pool is fuller than we thought */
                return NULL;
            }
            zorro_area[i].p = p;
            zorro_area[i].size = size;
            zorro_avail -= size;
            return p;
        }
    }
    return NULL;
}

void vixen_zorro_free(void *ptr) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(zorro_area); i++) {
        if (zorro_area[i].p == ptr) {
            zorro_avail += zorro_area[i].size;
            zorro_area[i].p = NULL;
            ksys_alloc_free(ptr);
            return;
        }
    }
}

void gc_collect(void) {
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}

void nlr_jump_fail(void *val) {
    mp_printf(&mp_plat_print, "uncaught NLR\n");
    exit(1);
}

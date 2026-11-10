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

void vixen_disk_automount(void);
void vixen_disk_enter_app_dir(void);
#define VIXEN_APP_DIR "/sd/vixen-32/apps/micropython"
#if VIXEN_LVGL
#include "vx_lvgl.h"
#endif
#if MICROPY_PY_NETWORK
#include "extmod/modnetwork.h"
#endif

void vixen_random_init(void);
void vixen_random_boot_seed(void);

int main(int argc, char **argv) {
    static bool seeded = false;
    mp_cstack_init_with_sp_here(MICROPY_STACK_SIZE);
    vixen_random_init();
    memset(_GC_HEAP_START, 0, MICROPY_HEAP_SIZE);
    gc_init(_GC_HEAP_START, _GC_HEAP_START + MICROPY_HEAP_SIZE);

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

void gc_collect(void) {
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}

void nlr_jump_fail(void *val) {
    mp_printf(&mp_plat_print, "uncaught NLR\n");
    exit(1);
}

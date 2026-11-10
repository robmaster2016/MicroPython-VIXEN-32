/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 VIXEN-32 MicroPython port contributors
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

/* Python side of the LVGL display driver (vixen.lv_display / vixen.lv_dither)
 * and the LVGL main loop.
 *
 *     import lvgl as lv, vixen    # the import runs lv_init()
 *     vixen.lv_display()          # 384x224 display + mouse; console hidden
 *     btn = lv.button(lv.screen_active())
 *
 * There is no main loop to write: lv_timer_handler() runs every
 * LV_HANDLER_PERIOD_MS from the console poll (uart_poll(), called while the
 * REPL waits, in time.sleep() and every 64 bytecodes) via the MicroPython
 * scheduler. Scheduled callbacks run only between bytecodes, never inside a C
 * call, so LVGL is never re-entered from an LVGL call Python is making.
 *
 * F1 switches between the GUI and the REPL console (handled in uart.c).
 *
 * Keys: normally the keyboard types into the REPL. vixen.lv_keys(handler)
 * hands every key pressed while the GUI is shown to handler(key) instead, as
 * an LVGL key code: lv.KEY.NEXT / PREV (TAB / shift+TAB), UP, DOWN, LEFT,
 * RIGHT, ENTER, BACKSPACE, ESC, or the character's code. vixen.lv_keys(None)
 * gives the keyboard back to the REPL. Ctrl-C / RUN/STOP always stop a program.
 */

#include "py/runtime.h"
#include "vx_lvgl.h"

#if VIXEN_LVGL

#include "lvgl/lvgl.h"

void uart_console_show(bool on);

#define LV_HANDLER_PERIOD_MS 5u

static bool lv_active;          /* vixen.lv_display() has run this session */
static bool lv_scheduled;       /* a handler call is queued */
static uint32_t lv_last_ms;

MP_REGISTER_ROOT_POINTER(mp_obj_t vixen_lv_key_handler);

bool vx_lvgl_mp_key_wanted(void) {
    mp_obj_t h = MP_STATE_VM(vixen_lv_key_handler);
    return lv_active && h != MP_OBJ_NULL && h != mp_const_none;
}

void vx_lvgl_mp_key(uint32_t key) {
    mp_sched_schedule(MP_STATE_VM(vixen_lv_key_handler), MP_OBJ_NEW_SMALL_INT(key));
}

/* vixen.lv_keys(handler): see the file comment. */
static mp_obj_t vixen_lv_keys(mp_obj_t handler) {
    if (handler != mp_const_none && !mp_obj_is_callable(handler)) {
        mp_raise_TypeError(MP_ERROR_TEXT("handler must be callable or None"));
    }
    MP_STATE_VM(vixen_lv_key_handler) = handler;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(vixen_lv_keys_obj, vixen_lv_keys);

static mp_obj_t lv_handler_task(mp_obj_t arg) {
    (void)arg;
    lv_scheduled = false;
    if (lv_active) {
        lv_timer_handler();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lv_handler_task_obj, lv_handler_task);

void vx_lvgl_mp_poll(uint32_t now_ms) {
    if (!lv_active || lv_scheduled || (uint32_t)(now_ms - lv_last_ms) < LV_HANDLER_PERIOD_MS) {
        return;
    }
    lv_last_ms = now_ms;
    lv_scheduled = mp_sched_schedule(MP_OBJ_FROM_PTR(&lv_handler_task_obj), mp_const_none);
}

void mp_lv_deinit_gc(void);

/* Soft reset: the gc heap -- and with it everything LVGL allocated, including
 * its globals (LV_GLOBAL_CUSTOM lives in the heap) -- is gone. The binding's
 * "module initialised" flag survives it, though, so the next `import lvgl`
 * would skip lv_init() and run on freed memory: reset it like lv_deinit's
 * GC hook does. */
void vx_lvgl_mp_reset(void) {
    mp_lv_deinit_gc();
    if (lv_active) {
        vx_lvgl_display_off();
        uart_console_show(true);
    }
    lv_active = false;
    lv_scheduled = false;
    MP_STATE_VM(vixen_lv_key_handler) = MP_OBJ_NULL;
}

/* vixen.lv_display(dither=False): create the LVGL display and mouse. */
static mp_obj_t vixen_lv_display(size_t n_args, const mp_obj_t *args) {
    bool dither = n_args > 0 && mp_obj_is_true(args[0]);

    if (!lv_is_initialized()) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("import lvgl first"));
    }
    if (lv_active) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("display already created"));
    }
    vx_lvgl_display_init(dither);
    uart_console_show(false);
    lv_active = true;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(vixen_lv_display_obj, 0, 1, vixen_lv_display);

/* vixen.lv_dither(on): 4x4 ordered dithering of the 256-colour output. */
static mp_obj_t vixen_lv_dither(mp_obj_t on) {
    if (!lv_active) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("no display"));
    }
    vx_lvgl_set_dither(mp_obj_is_true(on));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(vixen_lv_dither_obj, vixen_lv_dither);

#endif // VIXEN_LVGL

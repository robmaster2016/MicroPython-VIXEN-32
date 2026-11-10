/* SPDX-License-Identifier: MIT
 * Copyright (c) 2025-2026 Robert S. (aka Robmaster) */
// VIXEN-32 LVGL display + mouse driver (LVGL build). See vx_lvgl.c.
#ifndef MICROPY_INCLUDED_VIXEN_VX_LVGL_H
#define MICROPY_INCLUDED_VIXEN_VX_LVGL_H

#include <stdbool.h>
#include <stdint.h>

#define VX_DISP_W 384
#define VX_DISP_H 224

// Create the LVGL display and mouse input device (after lv.init()).
void vx_lvgl_display_init(bool dither);

// 4x4 ordered dithering on/off (redraws the screen).
void vx_lvgl_set_dither(bool on);

// Hide the GFX layer and the mouse cursor (soft reset).
void vx_lvgl_display_off(void);

// Python glue (vx_lvgl_mp.c): drive lv_timer_handler from the console poll,
// and forget the display on soft reset.
void vx_lvgl_mp_poll(uint32_t now_ms);
void vx_lvgl_mp_reset(void);

// Keys for the GUI (vixen.lv_keys): is a Python handler registered and the
// display up? Deliver one LVGL key code to it (scheduled, not called here).
bool vx_lvgl_mp_key_wanted(void);
void vx_lvgl_mp_key(uint32_t key);

#endif // MICROPY_INCLUDED_VIXEN_VX_LVGL_H

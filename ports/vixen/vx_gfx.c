/* SPDX-License-Identifier: MIT
 * Copyright (c) 2025-2026 Robert S. (aka Robmaster) */
/* gfx: the VIXEN-32 blitter from Python (LVGL builds).
 *
 *     import gfx
 *     gfx.open()                          # 368 x 216, double buffered
 *     gfx.zbuffer(True)
 *     gfx.clear(0)
 *     gfx.tri((10, 10, 2), (200, 30, 2), (90, 180, 5), gfx.rgb(255, 128, 0))
 *     gfx.tri_shade((x, y, z, colour), ...)          # Gouraud
 *     gfx.tri_tex((x, y, z, u, v), ..., tex, persp=True)
 *     gfx.flip()
 *
 * Every triangle is ONE blitter op (core v0.93): the blitter walks its rows
 * itself, Z-tested when the Z buffer (core v0.90) is on, perspective-correct
 * with persp=True (core v0.91). Ops go into the blitter's 256-deep queue
 * (core v0.85), so Python sets up the next triangle while the blitter draws.
 *
 * The screen is the console's area inside its margins (vixen.screen_size()),
 * in the GFX layer, like the LVGL display -- and it uses LVGL's memory: two
 * of LVGL's framebuffers (chip 0x300000, 0x320000), the third (0x340000,
 * 128 KB) holds textures. So gfx and vixen.lv_display() exclude each other
 * within a session. The palette is LVGL's 6x6x6 colour cube + 40 greys;
 * gfx.rgb(r, g, b) gives the nearest index.
 *
 * z is the distance (> 0, smaller = nearer; only used with the Z buffer and
 * persp). The blitter keeps 1/z: we pass ZK / z, so z from 1 to ~30000
 * works, finer near the eye. Coordinates are floats or ints, in pixels from
 * the top left of the screen; triangles are clipped to it, rects and lines
 * too.
 */

#include <math.h>
#include <string.h>

#include "py/runtime.h"
#include "py/obj.h"
#include "py/gc.h"

#if VIXEN_LVGL

#include "vih_regs.h"
#include "vih_tri.h"
#include "uart.h"
#include "vx_lvgl.h"

uint64_t ticks_raster_lines(void);
void uart_console_show(bool on);

#define GFX_STRIDE   512u
#define GFX_FB0      0x300000u
#define GFX_FB1      0x320000u
#define GFX_TEX      0x340000u          /* 128 KB of textures */
#define GFX_TEX_PAGE 4096u
#define GFX_TEX_PAGES 32u
#define GFX_LATCH_LINE 226u             /* as vx_lvgl.c's VX_LATCH_LINE */
#define ZK           32767.0f           /* 1/z scale: |u*w| < 2^23 for persp */

#define CPU_PTR(a) ((volatile uint8_t *)vih_sdram_ptr(a))

static bool gfx_open;
static bool gfx_z;
static uint32_t gfx_w, gfx_h, gfx_left, gfx_top;
static uint32_t gfx_back;               /* chip address of the back buffer */
static uint64_t gfx_latch;              /* raster line by which the last flip latched */
static uint32_t gfx_tex_loaded;         /* chip address of the texture in the cache, 0 = none */
static uint32_t tex_pages;              /* bit n = page n of GFX_TEX taken */

static void colours_reset(void);

static void check_open(void) {
    if (!gfx_open) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("gfx.open() first"));
    }
}

/* Before drawing into the back buffer: the flip that made it the back buffer
 * must have latched (the reader no longer shows it). */
static uint32_t back_row0(void) {
    check_open();
    while (ticks_raster_lines() < gfx_latch) {
    }
    return gfx_back + gfx_top * GFX_STRIDE;
}

static int32_t get_int(mp_obj_t o) {
    return mp_obj_is_float(o) ? (int32_t)floorf(mp_obj_get_float(o)) : mp_obj_get_int(o);
}

/* ---- open / close / flip ------------------------------------------------ */

static void set_zbase(void) {
    vih_blit_wait();
    vih_blit_zbase(gfx_back + gfx_top * GFX_STRIDE);
}

static mp_obj_t gfx_open_fn(void) {
    if (vx_lvgl_mp_active()) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("LVGL has the screen (vixen.lv_display())"));
    }
    uint32_t m[4];
    uart_console_margins(m);
    gfx_left = m[0];
    gfx_top = m[2];
    gfx_w = uart_console_width();
    gfx_h = uart_console_height();
    vx_lvgl_palette_load();
    colours_reset();
    vih_blit_queue(1u);
    vih_blit_tri_clip(0u, gfx_w);
    for (uint32_t i = 0u; i < 2u; i++) {
        uint32_t fb = i ? GFX_FB1 : GFX_FB0;
        vih_blit_fill(fb, 0u, 256u, 256u, 0u);
        vih_blit_wait();
        vih_blit_fill(fb + 256u, 256u, 256u, 256u, 0u);
        vih_blit_wait();
    }
    /* framebuffer column 0 at screen x = left, as vx_lvgl.c does */
    vih_gfx_scroll((512u - gfx_left) & 0x1FFu, 0u);
    vih_gfx_ptr_addr(GFX_FB0);
    gfx_back = GFX_FB1;
    gfx_latch = 0u;
    gfx_tex_loaded = 0u;
    gfx_open = true;
    set_zbase();
    vih_blit_zclear();
    vih_blit_z(gfx_z ? 1u : 0u);
    uart_console_show(false);
    VIH_GFX_ENABLE = 1u;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gfx_open_obj, gfx_open_fn);

/* palette banks, the LUT and the colour base back to their defaults */
static void colours_reset(void) {
    vih_blit_wait();
    VIH_PAL_SEL = 0u;
    vih_blit_lut_enable(0u);
    VIH_BLIT_CBASE = 0u;
}

void vx_gfx_reset(void) {
    if (gfx_open) {
        vih_blit_wait();
        VIH_GFX_ENABLE = 0u;
        vih_blit_z(0u);
        vih_blit_queue(0u);
        colours_reset();
        uart_console_show(true);
    }
    gfx_open = false;
    gfx_z = false;
}

static mp_obj_t gfx_close_fn(void) {
    vx_gfx_reset();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gfx_close_obj, gfx_close_fn);

/* gfx.flip(): show what was drawn. Waits for the blitter; the new picture
 * is taken at the next frame start, and drawing waits for that by itself. */
static mp_obj_t gfx_flip_fn(void) {
    check_open();
    vih_blit_wait();
    uint32_t vpos = VIH_RASTER_VPOS;
    uint64_t now = ticks_raster_lines();
    vih_gfx_ptr_addr(gfx_back);
    gfx_latch = now + (GFX_LATCH_LINE + 312u - (vpos % 312u)) % 312u + 2u;
    gfx_back = (gfx_back == GFX_FB0) ? GFX_FB1 : GFX_FB0;
    set_zbase();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gfx_flip_obj, gfx_flip_fn);

static mp_obj_t gfx_size_fn(void) {
    uint32_t w = gfx_open ? gfx_w : uart_console_width();
    uint32_t h = gfx_open ? gfx_h : uart_console_height();
    mp_obj_t t[2] = { MP_OBJ_NEW_SMALL_INT(w), MP_OBJ_NEW_SMALL_INT(h) };
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(gfx_size_obj, gfx_size_fn);

/* gfx.zbuffer(on): Z test for clear-to-draw ops (rect, triangles). */
static mp_obj_t gfx_zbuffer_fn(mp_obj_t on) {
    gfx_z = mp_obj_is_true(on);
    if (gfx_open) {
        vih_blit_wait();
        vih_blit_zclear();
        vih_blit_z(gfx_z ? 1u : 0u);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gfx_zbuffer_obj, gfx_zbuffer_fn);

/* gfx.rgb(r, g, b): the nearest palette index. */
/* ---- palettes, LUT, colour base ------------------------------------------ */

static uint32_t get_bank(mp_obj_t o) {
    mp_int_t b = mp_obj_get_int(o);
    if (b < 0 || b > 7) {
        mp_raise_ValueError(MP_ERROR_TEXT("bank 0..7"));
    }
    return (uint32_t)b;
}

static uint32_t get_byte(mp_obj_t o) {
    mp_int_t v = mp_obj_get_int(o);
    return v < 0 ? 0u : v > 255 ? 255u : (uint32_t)v;
}

/* gfx.palette(i, r, g, b, bank=0): entry i (0-255) of palette bank 0-7
 * (core v0.95) = r, g, b (0-255 each, kept as RGB565). Bank 0 is the one
 * the screen shows unless gfx.palette_show() says otherwise; changing an
 * entry the screen shows recolours every pixel of that index at once. */
static mp_obj_t gfx_palette_fn(size_t n_args, const mp_obj_t *pos, mp_map_t *kw) {
    enum { ARG_i, ARG_r, ARG_g, ARG_b, ARG_bank };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_i, MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_r, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_g, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_b, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_bank, MP_ARG_INT, { .u_int = 0 } },
    };
    mp_arg_val_t a[5];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    if (a[ARG_i].u_int < 0 || a[ARG_i].u_int > 255) {
        mp_raise_ValueError(MP_ERROR_TEXT("index 0..255"));
    }
    uint32_t r = get_byte(a[ARG_r].u_obj), g = get_byte(a[ARG_g].u_obj), b = get_byte(a[ARG_b].u_obj);
    vih_set_palette8(get_bank(MP_OBJ_NEW_SMALL_INT(a[ARG_bank].u_int)), (uint32_t)a[ARG_i].u_int,
        ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(gfx_palette_obj, 4, gfx_palette_fn);

/* gfx.palette_cube(bank): the standard palette (6x6x6 colour cube + 40
 * greys, what gfx.rgb() picks from) into bank 0-7. */
static mp_obj_t gfx_palette_cube_fn(mp_obj_t bank) {
    vx_lvgl_palette_bank(get_bank(bank));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gfx_palette_cube_obj, gfx_palette_cube_fn);

/* gfx.palette_show(bank): the picture shows palette bank 0-7, from the next
 * frame -- colour cycling and fades without redrawing. The border stays in
 * bank 0 (black). */
static mp_obj_t gfx_palette_show_fn(mp_obj_t bank) {
    uint32_t b = get_bank(bank);
    VIH_PAL_SEL = vih_pal_sel_value(b, 0u, 0u, 0u);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gfx_palette_show_obj, gfx_palette_show_fn);

/* gfx.lut(table): blit() and tri_tex() write table[pixel] instead of the
 * pixel (after color_base; a key colour still means transparent): 256 bytes,
 * or None to switch it off. Recolour sprites and textures without copying
 * them. Waits for the blitter: the running op reads the table live. */
static mp_obj_t gfx_lut_fn(mp_obj_t table) {
    vih_blit_wait();
    if (table == mp_const_none) {
        vih_blit_lut_enable(0u);
        return mp_const_none;
    }
    mp_buffer_info_t b;
    mp_get_buffer_raise(table, &b, MP_BUFFER_READ);
    if (b.len != 256u) {
        mp_raise_ValueError(MP_ERROR_TEXT("256 bytes"));
    }
    const uint8_t *t = b.buf;
    for (uint32_t i = 0u; i < 256u; i++) {
        vih_blit_lut_set((uint8_t)i, t[i]);
    }
    vih_blit_lut_enable(1u);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gfx_lut_obj, gfx_lut_fn);

/* gfx.color_base(n): n (0-255) is added to every pixel blit() and tri_tex()
 * write (mod 256, after the key test): draw a 16-colour image into any
 * block of 16 entries, e.g. color_base(16 * k). 0 = off. */
static mp_obj_t gfx_color_base_fn(mp_obj_t n) {
    vih_blit_wait();
    VIH_BLIT_CBASE = get_byte(n);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gfx_color_base_obj, gfx_color_base_fn);

static mp_obj_t gfx_rgb_fn(mp_obj_t r, mp_obj_t g, mp_obj_t b) {
    vx_lvgl_palette_load();
    uint32_t c[3] = { (uint32_t)mp_obj_get_int(r), (uint32_t)mp_obj_get_int(g), (uint32_t)mp_obj_get_int(b) };
    for (int i = 0; i < 3; i++) {
        c[i] = c[i] > 255u ? 255u : c[i];
    }
    return MP_OBJ_NEW_SMALL_INT(vx_lvgl_rgb_index(c[0], c[1], c[2]));
}
static MP_DEFINE_CONST_FUN_OBJ_3(gfx_rgb_obj, gfx_rgb_fn);

/* ---- rectangles and lines ------------------------------------------------ */

static void fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c) {
    uint32_t row0 = back_row0();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int32_t)gfx_w) { w = (int32_t)gfx_w - x; }
    if (y + h > (int32_t)gfx_h) { h = (int32_t)gfx_h - y; }
    if (w <= 0 || h <= 0) {
        return;
    }
    /* drawn over everything, no Z test: the Z enable goes into the queue
     * with each op, so it can change between ops without a wait */
    vih_blit_wait_pend();
    vih_blit_z(0u);
    vih_blit_fill(row0 + (uint32_t)y * GFX_STRIDE + (uint32_t)x, (uint16_t)(GFX_STRIDE - (uint32_t)w),
        (uint16_t)w, (uint16_t)h, (uint8_t)c);
    vih_blit_z(gfx_z ? 1u : 0u);
}

/* gfx.clear(c=0): the whole screen, and the Z buffer. */
static mp_obj_t gfx_clear_fn(size_t n_args, const mp_obj_t *args) {
    uint32_t c = n_args ? (uint32_t)mp_obj_get_int(args[0]) : 0u;
    uint32_t row0 = back_row0();
    if (gfx_z) {
        vih_blit_wait();                /* nothing queued may still test the old Z */
        vih_blit_zclear();              /* ~0.43 ms, beside the fill; Z ops wait for it */
    }
    vih_blit_wait_pend();
    vih_blit_z(0u);
    vih_blit_fill(row0, (uint16_t)(GFX_STRIDE - gfx_w), (uint16_t)gfx_w, (uint16_t)gfx_h, (uint8_t)c);
    vih_blit_z(gfx_z ? 1u : 0u);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gfx_clear_obj, 0, 1, gfx_clear_fn);

static mp_obj_t gfx_rect_fn(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    fill_rect(get_int(args[0]), get_int(args[1]), get_int(args[2]), get_int(args[3]),
        (uint32_t)mp_obj_get_int(args[4]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gfx_rect_obj, 5, 5, gfx_rect_fn);

/* Liang-Barsky against 0..w-1, 0..h-1 */
static bool clip_line(float *x0, float *y0, float *x1, float *y1) {
    float t0 = 0.0f, t1 = 1.0f, dx = *x1 - *x0, dy = *y1 - *y0;
    float p[4] = { -dx, dx, -dy, dy };
    float q[4] = { *x0, (float)gfx_w - 1.0f - *x0, *y0, (float)gfx_h - 1.0f - *y0 };
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f) {
                return false;
            }
        } else {
            float t = q[i] / p[i];
            if (p[i] < 0.0f) {
                if (t > t1) { return false; }
                if (t > t0) { t0 = t; }
            } else {
                if (t < t0) { return false; }
                if (t < t1) { t1 = t; }
            }
        }
    }
    float ax = *x0 + t0 * dx, ay = *y0 + t0 * dy;
    *x1 = *x0 + t1 * dx;
    *y1 = *y0 + t1 * dy;
    *x0 = ax;
    *y0 = ay;
    return true;
}

static mp_obj_t gfx_line_fn(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    float x0 = mp_obj_get_float(args[0]), y0 = mp_obj_get_float(args[1]);
    float x1 = mp_obj_get_float(args[2]), y1 = mp_obj_get_float(args[3]);
    uint32_t c = (uint32_t)mp_obj_get_int(args[4]);
    uint32_t row0 = back_row0();
    if (!clip_line(&x0, &y0, &x1, &y1)) {
        return mp_const_none;
    }
    vih_blit_wait_pend();
    vih_blit_line(row0, GFX_STRIDE, (int32_t)lroundf(x0), (int32_t)lroundf(y0),
        (int32_t)lroundf(x1), (int32_t)lroundf(y1), (uint8_t)c);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gfx_line_obj, 5, 5, gfx_line_fn);

/* ---- textures ------------------------------------------------------------ */

typedef struct {
    mp_obj_base_t base;
    uint32_t addr;                      /* chip address, 0 = freed */
    uint16_t w, h, stride;
    uint8_t page0, npages;
} gfx_tex_obj_t;

static const mp_obj_type_t gfx_tex_type;

/* gfx.Texture(data, w, h): w x h palette indices (bytes-like, row by row),
 * copied to chip RAM where the blitter reads them. Up to 256 x 256 for
 * triangles; 128 KB for all textures together. */
static mp_obj_t gfx_tex_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 3, 3, false);
    mp_buffer_info_t b;
    mp_get_buffer_raise(args[0], &b, MP_BUFFER_READ);
    uint32_t w = (uint32_t)mp_obj_get_int(args[1]), h = (uint32_t)mp_obj_get_int(args[2]);
    if (w == 0u || h == 0u || w > 384u || h > 256u || b.len < w * h) {
        mp_raise_ValueError(MP_ERROR_TEXT("w x h bytes, w <= 384, h <= 256"));
    }
    uint32_t stride = (w + 1u) & ~1u;   /* even: texload and copies want it */
    uint32_t n = (stride * h + GFX_TEX_PAGE - 1u) / GFX_TEX_PAGE;
    uint32_t mask = (n >= 32u) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    uint32_t p = 0u;
    for (int tries = 0;; tries++) {
        p = 0u;
        while (p + n <= GFX_TEX_PAGES && (tex_pages & (mask << p)) != 0u) {
            p++;
        }
        if (p + n <= GFX_TEX_PAGES) {
            break;
        }
        if (tries == 1) {
            mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("texture memory full (128 KB)"));
        }
        gc_collect();                   /* textures nobody holds free their pages */
    }
    tex_pages |= mask << p;

    #if MICROPY_ENABLE_FINALISER
    gfx_tex_obj_t *t = mp_obj_malloc_with_finaliser(gfx_tex_obj_t, type);
    #else
    gfx_tex_obj_t *t = mp_obj_malloc(gfx_tex_obj_t, type);
    #endif
    t->addr = GFX_TEX + p * GFX_TEX_PAGE;
    t->w = (uint16_t)w;
    t->h = (uint16_t)h;
    t->stride = (uint16_t)stride;
    t->page0 = (uint8_t)p;
    t->npages = (uint8_t)n;
    vih_blit_wait();                    /* a queued op may still read the old texture */
    volatile uint8_t *dst = CPU_PTR(t->addr);
    const uint8_t *src = b.buf;
    for (uint32_t y = 0u; y < h; y++) {
        for (uint32_t x = 0u; x < w; x++) {
            dst[y * stride + x] = src[y * w + x];
        }
    }
    if (gfx_tex_loaded == t->addr) {
        gfx_tex_loaded = 0u;
    }
    return MP_OBJ_FROM_PTR(t);
}

static mp_obj_t gfx_tex_del(mp_obj_t self_in) {
    gfx_tex_obj_t *t = MP_OBJ_TO_PTR(self_in);
    if (t->addr != 0u) {
        uint32_t mask = (t->npages >= 32u) ? 0xFFFFFFFFu : ((1u << t->npages) - 1u);
        tex_pages &= ~(mask << t->page0);
        if (gfx_tex_loaded == t->addr) {
            gfx_tex_loaded = 0u;
        }
        t->addr = 0u;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gfx_tex_del_obj, gfx_tex_del);

static void gfx_tex_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    gfx_tex_obj_t *t = MP_OBJ_TO_PTR(self_in);
    if (dest[0] != MP_OBJ_NULL) {
        return;
    }
    if (attr == MP_QSTR_width) {
        dest[0] = MP_OBJ_NEW_SMALL_INT(t->w);
    } else if (attr == MP_QSTR_height) {
        dest[0] = MP_OBJ_NEW_SMALL_INT(t->h);
    } else {
        dest[1] = MP_OBJ_SENTINEL;      /* look in locals_dict */
    }
}

static const mp_rom_map_elem_t gfx_tex_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&gfx_tex_del_obj) },
    { MP_ROM_QSTR(MP_QSTR_free), MP_ROM_PTR(&gfx_tex_del_obj) },
};
static MP_DEFINE_CONST_DICT(gfx_tex_locals, gfx_tex_locals_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    gfx_tex_type, MP_QSTR_Texture, MP_TYPE_FLAG_NONE,
    make_new, gfx_tex_make_new,
    attr, gfx_tex_attr,
    locals_dict, &gfx_tex_locals
    );

static gfx_tex_obj_t *get_tex(mp_obj_t o) {
    gfx_tex_obj_t *t = MP_OBJ_TO_PTR(mp_obj_cast_to_native_base(o, MP_OBJ_FROM_PTR(&gfx_tex_type)));
    if (t == NULL || t->addr == 0u) {
        mp_raise_TypeError(MP_ERROR_TEXT("a gfx.Texture"));
    }
    return t;
}

/* gfx.blit(tex, x, y, key=-1): copy a texture onto the screen; with key, that
 * index is transparent. */
static mp_obj_t gfx_blit_fn(size_t n_args, const mp_obj_t *args) {
    gfx_tex_obj_t *t = get_tex(args[0]);
    int32_t x = get_int(args[1]), y = get_int(args[2]);
    int32_t key = n_args > 3 ? mp_obj_get_int(args[3]) : -1;
    int32_t sx = 0, sy = 0, w = t->w, h = t->h;
    uint32_t row0 = back_row0();
    if (x < 0) { sx = -x; w += x; x = 0; }
    if (y < 0) { sy = -y; h += y; y = 0; }
    if (x + w > (int32_t)gfx_w) { w = (int32_t)gfx_w - x; }
    if (y + h > (int32_t)gfx_h) { h = (int32_t)gfx_h - y; }
    if (w <= 0 || h <= 0) {
        return mp_const_none;
    }
    vih_blit_wait_pend();
    vih_blit_copy_ex(t->addr + (uint32_t)sy * t->stride + (uint32_t)sx, (uint16_t)(t->stride - (uint32_t)w),
        row0 + (uint32_t)y * GFX_STRIDE + (uint32_t)x, (uint16_t)(GFX_STRIDE - (uint32_t)w),
        (uint16_t)w, (uint16_t)h, key >= 0, (uint8_t)key);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gfx_blit_obj, 3, 4, gfx_blit_fn);

/* ---- triangles ------------------------------------------------------------ */

/* (x, y[, z[, a[, b]]]) -> vertex; nval = values the op needs after x, y */
static float get_vertex(mp_obj_t o, float out[5], size_t need) {
    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(o, &n, &items);
    if (n < 2u || n < need) {
        mp_raise_ValueError(MP_ERROR_TEXT("vertex: (x, y, z, ...) too short"));
    }
    for (size_t i = 0; i < 5u; i++) {
        out[i] = i < n ? mp_obj_get_float(items[i]) : (i == 2u ? 1.0f : 0.0f);
    }
    if (out[2] < 1.0f) {
        out[2] = 1.0f;                  /* z: no closer than 1 */
    }
    return out[2];
}

static int32_t q16(float f) {
    return (int32_t)lroundf(f * 65536.0f);
}

/* kind 0 = flat (colour), 1 = Gouraud, 2 = texture, 3 = perspective texture */
static void draw_tri(const mp_obj_t *pts, int kind, uint32_t ctrl) {
    vih_tri_vtx_t v[3];
    for (int i = 0; i < 3; i++) {
        float f[5];
        get_vertex(pts[i], f, kind == 0 ? 2u : kind == 1 ? 4u : 5u);
        float w = ZK / f[2];            /* 1/z, scaled */
        v[i].x = q16(f[0]);
        v[i].y = q16(f[1]);
        v[i].z = (uint32_t)(w * 65536.0f);
        if (kind == 1) {
            v[i].u = q16(f[3]);
            v[i].v = 0;
        } else if (kind == 2) {
            v[i].u = q16(f[3]);
            v[i].v = q16(f[4]);
        } else if (kind == 3) {
            v[i].u = (int32_t)lroundf(f[3] * w * 256.0f);
            v[i].v = (int32_t)lroundf(f[4] * w * 256.0f);
        } else {
            v[i].u = v[i].v = 0;
        }
    }
    uint32_t row0 = back_row0();
    vih_blit_wait_pend();
    vih_tri_draw(&v[0], &v[1], &v[2], 0, (int32_t)gfx_h, row0, GFX_STRIDE, ctrl);
}

/* gfx.tri(p0, p1, p2, colour): p = (x, y) or (x, y, z) */
static mp_obj_t gfx_tri_fn(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    draw_tri(args, 0, VIH_TRI_FILL((uint32_t)mp_obj_get_int(args[3])));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gfx_tri_obj, 4, 4, gfx_tri_fn);

/* gfx.tri_shade(p0, p1, p2): p = (x, y, z, colour); the palette index runs
 * smoothly between the corners (a ramp of the palette, e.g. the greys
 * 216-255). */
static mp_obj_t gfx_tri_shade_fn(mp_obj_t a, mp_obj_t b, mp_obj_t c) {
    mp_obj_t p[3] = { a, b, c };
    draw_tri(p, 1, VIH_TRI_GOURAUD);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(gfx_tri_shade_obj, gfx_tri_shade_fn);

/* gfx.tri_tex(p0, p1, p2, tex, persp=False, key=False): p = (x, y, z, u, v),
 * u, v in texels; the texture repeats every 256. key: index 0 transparent. */
static mp_obj_t gfx_tri_tex_fn(size_t n_args, const mp_obj_t *pos, mp_map_t *kw) {
    enum { ARG_persp, ARG_key };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_persp, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_key, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
    };
    mp_arg_val_t a[2];
    if (n_args != 4u) {
        mp_raise_TypeError(MP_ERROR_TEXT("tri_tex(p0, p1, p2, tex, persp=False, key=False)"));
    }
    mp_arg_parse_all(0, NULL, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    gfx_tex_obj_t *t = get_tex(pos[3]);
    if (t->w > 256u) {
        mp_raise_ValueError(MP_ERROR_TEXT("texture wider than 256"));
    }
    back_row0();
    if (gfx_tex_loaded != t->addr) {
        vih_blit_wait();                /* queued texture ops read the cache live */
        vih_blit_texload(t->addr, t->stride, t->h, 0u);
        vih_blit_wait();
        gfx_tex_loaded = t->addr;
    }
    bool persp = a[ARG_persp].u_bool, key = a[ARG_key].u_bool;
    draw_tri(pos, persp ? 3 : 2,
        persp ? (key ? VIH_TRI_PERSP_KEY : VIH_TRI_PERSP) : (key ? VIH_TRI_TEX_KEY : VIH_TRI_TEX));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(gfx_tri_tex_obj, 4, gfx_tri_tex_fn);

static const mp_rom_map_elem_t gfx_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_gfx) },
    { MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&gfx_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&gfx_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_flip), MP_ROM_PTR(&gfx_flip_obj) },
    { MP_ROM_QSTR(MP_QSTR_size), MP_ROM_PTR(&gfx_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_zbuffer), MP_ROM_PTR(&gfx_zbuffer_obj) },
    { MP_ROM_QSTR(MP_QSTR_rgb), MP_ROM_PTR(&gfx_rgb_obj) },
    { MP_ROM_QSTR(MP_QSTR_palette), MP_ROM_PTR(&gfx_palette_obj) },
    { MP_ROM_QSTR(MP_QSTR_palette_cube), MP_ROM_PTR(&gfx_palette_cube_obj) },
    { MP_ROM_QSTR(MP_QSTR_palette_show), MP_ROM_PTR(&gfx_palette_show_obj) },
    { MP_ROM_QSTR(MP_QSTR_lut), MP_ROM_PTR(&gfx_lut_obj) },
    { MP_ROM_QSTR(MP_QSTR_color_base), MP_ROM_PTR(&gfx_color_base_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&gfx_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_rect), MP_ROM_PTR(&gfx_rect_obj) },
    { MP_ROM_QSTR(MP_QSTR_line), MP_ROM_PTR(&gfx_line_obj) },
    { MP_ROM_QSTR(MP_QSTR_tri), MP_ROM_PTR(&gfx_tri_obj) },
    { MP_ROM_QSTR(MP_QSTR_tri_shade), MP_ROM_PTR(&gfx_tri_shade_obj) },
    { MP_ROM_QSTR(MP_QSTR_tri_tex), MP_ROM_PTR(&gfx_tri_tex_obj) },
    { MP_ROM_QSTR(MP_QSTR_blit), MP_ROM_PTR(&gfx_blit_obj) },
    { MP_ROM_QSTR(MP_QSTR_Texture), MP_ROM_PTR(&gfx_tex_type) },
};
static MP_DEFINE_CONST_DICT(gfx_globals, gfx_globals_table);

const mp_obj_module_t gfx_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&gfx_globals,
};

MP_REGISTER_MODULE(MP_QSTR_gfx, gfx_module);

#endif

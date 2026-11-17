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

/* LVGL display + mouse driver for VIXEN-32 (LVGL build, make LVGL=1).
 *
 * Tuned on the MEGA65, without what MicroPython does itself (lv_init, the main loop, the
 * keyboard -- that belongs to the REPL here):
 *
 *  - LVGL renders RGB565 into a 384x24 partial buffer in BRAM (.bram_bss);
 *    the flush converts to a fixed 256-colour palette (6x6x6 cube + 40 greys)
 *    through a 64 KB table, or per pixel with 4x4 ordered dithering.
 *  - Three framebuffers in chip RAM rotate (triple buffering, flips latched
 *    once per frame); before a refresh draws into one, the areas the previous
 *    two refreshes changed are copied over from the front buffer by the
 *    blitter.
 *  - Areas are rounded to 4-pixel columns: the flush writes whole words only.
 *  - The display is the console's area inside its margins (uart.c: 368x216
 *    by default; vixen.console_margin() before vixen.lv_display() changes
 *    it). The GFX x scroll 512 - left puts framebuffer column 0 at screen
 *    x = left (the 512-pixel line is circular); LVGL draws from framebuffer
 *    row `top`; the mouse sprite gets both offsets. Before core v0.95 the
 *    picture also needed a fine X scroll of 4; the core fixed that.
 *  - The Amiga mouse in port 1 is an LVGL pointer; the cursor is hardware
 *    sprite channel 0, so moving it redraws nothing.
 *
 * Chip RAM (reserved in mcu/rv32/vixen_lvgl.ld): sprite slot 0xF0, the three
 * framebuffers at 0x300000/0x320000/0x340000.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"
#include "vih_regs.h"
#include "kernel_api.h"
#include "vx_lvgl.h"
#include "uart.h"

uintptr_t ticks_ms(void);
void ticks_poll(void);
uint64_t ticks_raster_lines(void);
void uart_console_lvgl_palette(void);

/* ---- configuration ------------------------------------------------------ */

#define FB_STRIDE        512u
#define FB_ADDR0         0x300000u
#define FB_ADDR1         0x320000u
#define FB_ADDR2         0x340000u
#define CURSOR_SLOT      0xF0u              /* chip address 0x0F0000 */
#define CURSOR_CHANNEL   0u

/* Raster line by which the reader has latched a GFX pointer written earlier. */
#ifndef VX_LATCH_LINE
#define VX_LATCH_LINE    226u
#endif

#define DRAW_BUF_LINES   24u
/* The display: the console's area inside its side margins (uart.c), taken
 * when the display is created. draw_buf is sized for the widest, 384. */
static int32_t disp_w = VX_DISP_W;
static int32_t disp_h = VX_DISP_H;
static uint32_t disp_left;
static uint32_t disp_top;
static uint32_t disp_org;               /* framebuffer offset of display row 0 */

static uint16_t draw_buf[VX_DISP_W * DRAW_BUF_LINES]
    __attribute__((aligned(8), section(".bram_bss")));

#define CPU_PTR(a) ((volatile uint8_t *)vih_sdram_ptr(a))

/* ---- palette: 6x6x6 colour cube + 40 greys ------------------------------ */

#define GREY_BASE   216u
#define GREY_STEPS  40u
#define SUM_MAX     189u            /* r5*2 + g6 + b5*2 */

static uint8_t lut_r[64 + 16];
static uint8_t lut_g[64 + 16];
static uint8_t lut_b[64 + 16];
static uint8_t lut_grey[SUM_MAX + 1u + 16u];

static void palette_init(void) {
    for (uint32_t i = 0u; i < 216u; i++) {
        uint32_t r = i / 36u, g = (i / 6u) % 6u, b = i % 6u;
        vih_set_palette(i, (r * 31u + 2u) / 5u, (g * 63u + 2u) / 5u, (b * 31u + 2u) / 5u);
    }
    for (uint32_t i = 0u; i < GREY_STEPS; i++) {
        uint32_t v = ((i + 1u) * 255u) / (GREY_STEPS + 1u);
        vih_set_palette(GREY_BASE + i, v >> 3, v >> 2, v >> 3);
    }
    for (uint32_t v = 0u; v < sizeof(lut_r); v++) {
        uint32_t c = (v > 63u) ? 63u : v;
        uint32_t level = (c * 5u + 31u) / 63u;
        lut_r[v] = (uint8_t)(level * 36u);
        lut_g[v] = (uint8_t)(level * 6u);
        lut_b[v] = (uint8_t)level;
    }
    for (uint32_t s = 0u; s < sizeof(lut_grey); s++) {
        uint32_t c = (s > SUM_MAX) ? SUM_MAX : s;
        uint32_t v = (c * 255u + SUM_MAX / 2u) / SUM_MAX;
        uint32_t k = (v * (GREY_STEPS + 1u) + 127u) / 255u;
        lut_grey[s] = (uint8_t)((k == 0u) ? 0u : (k > GREY_STEPS) ? 215u : GREY_BASE + k - 1u);
    }
}

/* ---- RGB565 -> palette index -------------------------------------------- */

static bool dither_on;

static const uint8_t bayer4[4][4] = {
    { 0u, 8u, 2u, 10u }, { 12u, 4u, 14u, 6u }, { 3u, 11u, 1u, 9u }, { 15u, 7u, 13u, 5u },
};

static inline uint32_t to_index(uint32_t c, uint32_t t) {
    uint32_t r = ((c >> 11) & 0x1Fu) << 1;
    uint32_t g = (c >> 5) & 0x3Fu;
    uint32_t b = (c & 0x1Fu) << 1;
    uint32_t hi = r > g ? r : g, lo = r < g ? r : g;

    hi = hi > b ? hi : b;
    lo = lo < b ? lo : b;
    if (hi - lo <= 3u) {
        uint32_t s = r + g + b + (t >> 2);
        return lut_grey[s > 2u ? s - 2u : 0u];
    }
    t = (t * 3u) >> 2;
    r += t;
    g += t;
    b += t;
    return lut_r[r > 6u ? r - 6u : 0u] + lut_g[g > 6u ? g - 6u : 0u] + lut_b[b > 6u ? b - 6u : 0u];
}

/* For the gfx module (vx_gfx.c): the same palette, and a colour's index. */
static bool palette_loaded;

void vx_lvgl_palette_load(void) {
    if (!palette_loaded) {
        palette_loaded = true;
        palette_init();
        uart_console_lvgl_palette();
    }
}

/* The same 256 colours into palette `bank` (0-7, core v0.95). */
void vx_lvgl_palette_bank(uint32_t bank) {
    for (uint32_t i = 0u; i < 216u; i++) {
        uint32_t r = i / 36u, g = (i / 6u) % 6u, b = i % 6u;
        vih_set_palette8(bank, i, (((r * 31u + 2u) / 5u) << 11) | (((g * 63u + 2u) / 5u) << 5) | ((b * 31u + 2u) / 5u));
    }
    for (uint32_t i = 0u; i < GREY_STEPS; i++) {
        uint32_t v = ((i + 1u) * 255u) / (GREY_STEPS + 1u);
        vih_set_palette8(bank, GREY_BASE + i, ((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3));
    }
}

uint32_t vx_lvgl_rgb_index(uint32_t r, uint32_t g, uint32_t b) {
    return to_index(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3), 8u);
}

/* RGB565 -> palette index, 64 KB, read once per pixel: in Trapdoor RAM
 * (core v0.87+, on-chip, 2 clocks a read, uncached), taken from the kernel's
 * allocator once per power-up. It used to sit in BRAM, where those 64 KB now
 * hold code. Without Trapdoor RAM (older core or kernel): chip RAM, read
 * through the data cache -- it works, a little slower, as the table is
 * twice the cache size. */
static uint8_t lut565_chip[65536];
static uint8_t *lut565;

static void lut565_init(void) {
    if (lut565 == NULL) {
        if (KERNEL_ENTRY(KENT_SYS_ALLOC_MEM) != NULL && vih_trapdoor_present()) {
            lut565 = ksys_alloc_mem(KMEM_TRAPDOOR, 0u, 65536u, 4u);
        }
        if (lut565 == NULL) {
            lut565 = lut565_chip;
        }
    }
    for (uint32_t c = 0u; c < 65536u; c++) {
        lut565[c] = (uint8_t)to_index(c, 8u);
        if ((c & 0x3FFu) == 0u) {
            ticks_poll();
        }
    }
}

static void convert_row(uint32_t fb, int32_t x, int32_t y, const uint16_t *src, uint32_t n) {
    uint32_t addr = fb + (uint32_t)y * FB_STRIDE + (uint32_t)x;
    volatile uint8_t *dst = CPU_PTR(addr);
    const uint8_t *t = bayer4[y & 3];
    uint32_t i = 0u;

#define PIX(k) (dither_on ? to_index(src[k], t[(x + (int32_t)(k)) & 3]) : (uint32_t)lut565[src[k]])
    while (i < n && ((addr + i) & 3u) != 0u) {
        dst[i] = (uint8_t)PIX(i);
        i++;
    }
    for (; i + 4u <= n; i += 4u) {
        *(volatile uint32_t *)(dst + i) = PIX(i) | (PIX(i + 1u) << 8) | (PIX(i + 2u) << 16) | (PIX(i + 3u) << 24);
    }
    for (; i < n; i++) {
        dst[i] = (uint8_t)PIX(i);
    }
#undef PIX
}

/* ---- triple buffering ---------------------------------------------------- */

static const uint32_t fb_addr[3] = { FB_ADDR0, FB_ADDR1, FB_ADDR2 };
static uint32_t front;
static uint32_t target;
static bool target_ready;
static bool shown;                     /* GFX layer switched on (first flip) */

#define MAX_DIRTY 24
typedef struct {
    lv_area_t a[MAX_DIRTY];
    uint32_t n;
    bool all;
    uint64_t latch_lines;
} dirty_t;
static dirty_t dirty_cur;
static dirty_t dirty_hist[2];

static void dirty_add(dirty_t *d, const lv_area_t *a) {
    if (d->all) {
        return;
    }
    if (d->n == MAX_DIRTY) {
        d->all = true;
        return;
    }
    d->a[d->n++] = *a;
}

static void blit_rect(uint32_t src_fb, uint32_t dst_fb, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    uint32_t off = (uint32_t)y1 * FB_STRIDE + (uint32_t)x1;
    uint16_t w = (uint16_t)(x2 - x1 + 1);
    uint16_t h = (uint16_t)(y2 - y1 + 1);

    vih_blit_wait_pend();
    vih_blit_copy(src_fb + off, (uint16_t)(FB_STRIDE - w), dst_fb + off, (uint16_t)(FB_STRIDE - w), w, h);
}

static void sync_dirty(const dirty_t *d, uint32_t src, uint32_t dst) {
    if (d->all) {
        blit_rect(src, dst, 0, 0, disp_w - 1, disp_h - 1);
        return;
    }
    for (uint32_t i = 0u; i < d->n; i++) {
        blit_rect(src, dst, d->a[i].x1, d->a[i].y1, d->a[i].x2, d->a[i].y2);
    }
}

static void prepare_target(void) {
    target = (front + 1u) % 3u;
    while (ticks_raster_lines() < dirty_hist[1].latch_lines) {
    }
    sync_dirty(&dirty_hist[1], fb_addr[front] + disp_org, fb_addr[target] + disp_org);
    sync_dirty(&dirty_hist[0], fb_addr[front] + disp_org, fb_addr[target] + disp_org);
    vih_blit_wait();
    target_ready = true;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint32_t w = (uint32_t)lv_area_get_width(area);
    uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);

    if (!target_ready) {
        prepare_target();
    }
    for (int32_t y = area->y1; y <= area->y2; y++) {
        convert_row(fb_addr[target] + disp_org, area->x1, y, (const uint16_t *)px_map, w);
        px_map += stride;
        ticks_poll();
    }
    dirty_add(&dirty_cur, area);

    if (lv_display_flush_is_last(disp)) {
        uint64_t now = ticks_raster_lines();
        uint32_t vpos = VIH_RASTER_VPOS;
        uint32_t ahead;

        vih_gfx_ptr_addr(fb_addr[target]);
        if (!shown) {
            VIH_GFX_ENABLE = 1u;
            shown = true;
        }
        front = target;
        target_ready = false;
        ahead = (VX_LATCH_LINE + 312u - (vpos % 312u)) % 312u;
        dirty_cur.latch_lines = now + ahead + 2u;
        dirty_hist[1] = dirty_hist[0];
        dirty_hist[0] = dirty_cur;
        dirty_cur.n = 0u;
        dirty_cur.all = false;
    }
    lv_display_flush_ready(disp);
}

/* Round areas out to 4-pixel columns: whole-word flush and blitter copies. */
static void rounder_cb(lv_event_t *e) {
    lv_area_t *a = lv_event_get_param(e);

    a->x1 &= ~3;
    a->x2 |= 3;
}

void vx_lvgl_set_dither(bool on) {
    dither_on = on;
    lv_obj_invalidate(lv_screen_active());
}

/* ---- mouse + hardware cursor -------------------------------------------- */

static int32_t mouse_x = VX_DISP_W / 2, mouse_y = VX_DISP_H / 2;   /* set again at init */
static uint32_t mouse_last;

static const char *const cursor_art[] = {
    "#", "##", "#.#", "#..#", "#...#", "#....#", "#.....#", "#......#",
    "#.......#", "#........#", "#.........#", "#......####", "#...#..#",
    "#..##..#", "#.# #..#", "##   #..#", "#    #..#", "      #..#", "       ##",
};

static void cursor_init(void) {
    volatile uint8_t *img = CPU_PTR(CURSOR_SLOT * 4096u);

    for (uint32_t i = 0u; i < 64u * 64u; i++) {
        img[i] = 0u;
    }
    for (uint32_t y = 0u; y < sizeof(cursor_art) / sizeof(cursor_art[0]); y++) {
        for (uint32_t x = 0u; cursor_art[y][x] != '\0'; x++) {
            char c = cursor_art[y][x];
            if (c == '#') {
                img[y * 64u + x] = (uint8_t)(GREY_BASE + 4u);
            } else if (c == '.') {
                img[y * 64u + x] = 215u;
            }
        }
    }
    vih_sprite_attr(CURSOR_CHANNEL, CURSOR_SLOT, 0u, VIH_SPR_8BPP | VIH_SPR_ON_TOP);
    vih_sprite_pos(CURSOR_CHANNEL, mouse_x + (int32_t)disp_left, mouse_y + (int32_t)disp_top);
    vih_sprite_enable_mask(1u << CURSOR_CHANNEL);
    mouse_last = VIH_MOUSE;
}

static void mouse_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    vih_mouse_t m = vih_mouse();
    vih_mouse_t last = { .raw = mouse_last };
    int32_t dx = vih_mouse_delta(m.f.x, last.f.x);
    int32_t dy = vih_mouse_delta(m.f.y, last.f.y);

    (void)indev;
    mouse_last = m.raw;
    mouse_x = LV_CLAMP(0, mouse_x + dx, disp_w - 1);
    mouse_y = LV_CLAMP(0, mouse_y + dy, disp_h - 1);
    if (dx != 0 || dy != 0) {
        vih_sprite_pos(CURSOR_CHANNEL, mouse_x + (int32_t)disp_left, mouse_y + (int32_t)disp_top);
    }
    data->point.x = mouse_x;
    data->point.y = mouse_y;
    data->state = m.f.lmb ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* ---- set-up ------------------------------------------------------------- */

static uint32_t tick_cb(void) {
    return (uint32_t)ticks_ms();
}

void vx_lvgl_display_init(bool dither) {
    lv_display_t *disp;
    lv_indev_t *mouse;

    {
        uint32_t m[4];
        uart_console_margins(m);
        disp_left = m[0];
        disp_top = m[2];
    }
    disp_w = (int32_t)uart_console_width();
    disp_h = (int32_t)uart_console_height();
    disp_org = disp_top * FB_STRIDE;
    mouse_x = disp_w / 2;
    mouse_y = disp_h / 2;

    /* hardware: palette, three cleared framebuffers, scroll, cursor */
    VIH_GFX_ENABLE = 0u;
    palette_init();
    palette_loaded = true;
    uart_console_lvgl_palette();       /* the console's colours, in the new palette */
    for (uint32_t i = 0u; i < 3u; i++) {
        for (uint32_t half = 0u; half < 2u; half++) {
            vih_blit_wait();
            vih_blit_fill(fb_addr[i] + half * 256u, 256u, 256u, 256u, 0u);
        }
    }
    vih_blit_wait();
    vih_gfx_ptr_addr(FB_ADDR0);
    /* Framebuffer column 0 to screen x = the left margin: the 512-pixel line
     * is circular, so x scroll 512 - left shows columns 512 - left..511
     * (blank, under the border) first. The top margin is a row offset
     * instead (disp_org): LVGL draws from framebuffer row `top`. */
    vih_gfx_scroll((512u - disp_left) & 0x1FFu, 0u);
    front = 0u;
    target_ready = false;
    shown = false;
    dirty_cur.n = 0u;
    dirty_cur.all = false;
    dirty_hist[0] = dirty_cur;
    dirty_hist[1] = dirty_cur;
    dither_on = dither;
    lut565_init();
    cursor_init();

    /* LVGL: lv.init() has run in Python */
    lv_tick_set_cb(tick_cb);
    disp = lv_display_create(disp_w, disp_h);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    mouse = lv_indev_create();
    lv_indev_set_type(mouse, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(mouse, mouse_read_cb);
}

void vx_lvgl_display_off(void) {
    VIH_GFX_ENABLE = 0u;
    vih_sprite_enable_mask(0u);
    shown = false;
}

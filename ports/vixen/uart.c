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

/* VIXEN-32 console: the VIH text/tile layer (output) + the VIH keyboard FIFO
 * (input). VIXEN has no UART. All registers are in the VIH control window
 * (0x3A000000), see the devkit common/vih_regs.h.
 *
 * Output follows the tut1 pattern exactly: the tile layer is a grid of 8x8
 * cells, one 32-bit word each (vih_tile_cell_t: ch = ASCII, fg/bg = palette
 * indices), written straight into VIH_TILE_MAP0_BASE with the built-in ROM
 * font (font_src=0, 8x8, the reset default -- no font load needed). We keep a
 * software screen (cells + cursor) and repaint the tile map on every change.
 *
 * Input polls VIH_KEYBOARD (PS/2 set-2 scancode FIFO): read the head, check
 * .f.valid/.f.pressed, collapse the E0/F0 prefixes, translate the make code to
 * ASCII (shift/caps handled), and pop.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "py/runtime.h"
#include "shared/runtime/interrupt_char.h"
#include "uart.h"
#include "vih_regs.h"

uintptr_t ticks_ms(void);

/* ---- text screen geometry -------------------------------------------------
 * The whole visible tile grid: 48x28 cells of 8x8 = 384x224 px, no border.
 *
 * History: the first console was 40x25 at cell (0,0) with the default border,
 * which hid the outer cells (the prompt was invisible); then it sat at cell
 * (2,3) inside a border. The top tile row always looked cut by ~4 lines. The
 * cause is the tile scroll: vih_init_registers() sets it to x=1, y=4 pixels,
 * which moves the whole text layer 4 lines up and 1 pixel left. With the
 * scroll at 0,0 (TILE_SCROLL_X/Y) the grid sits flush and needs no border.
 * vixen.tile_scroll(x, y) adjusts it at run time, should the hardware need
 * an offset after all (the GFX layer needs a fine X scroll of 4). */
#define CON_COLS 48u
#define CON_ROWS 28u
#define CON_X0   0u
#define CON_Y0   0u

#ifndef TILE_SCROLL_X
#define TILE_SCROLL_X 0u
#endif
#ifndef TILE_SCROLL_Y
#define TILE_SCROLL_Y 0u
#endif

/* C64-style colours (tut1): light-blue text on a blue background. These are
 * palette INDICES. The LVGL build replaces the whole palette with its colour
 * cube (vx_lvgl.c), where 6/14/15 are dark greens; from then on the console
 * uses the cube's nearest entries instead (uart_console_lvgl_palette()). */
static uint8_t con_fg = 14u;
static uint8_t con_bg = 6u;
static uint8_t con_curc = 15u;
#define CON_FG   con_fg
#define CON_BG   con_bg
#define CON_CUR  con_curc

/* ---- software screen ------------------------------------------------------ */
static uint32_t con_cells[CON_ROWS][CON_COLS];
static uint8_t con_cx;
static uint8_t con_cy;
static uint8_t con_cur_on;        /* cursor blink phase: 1 = visible */
static uint32_t con_blink_ms;     /* next blink toggle (ms) */

static inline uint32_t con_cell(uint8_t ch, uint8_t fg, uint8_t bg) {
    vih_tile_cell_t c;
    c.raw = 0u;
    c.f.ch = ch;
    c.f.fg = fg;
    c.f.bg = bg;
    return c.raw;
}

static void con_clear(void);

static void con_redraw(void) {
    volatile uint32_t *map = VIH_TILE_MAP0_BASE;
    uint8_t r, c;

    for (r = 0; r < CON_ROWS; r++) {
        for (c = 0; c < CON_COLS; c++) {
            map[(uint32_t)(r + CON_Y0) * 64u + c + CON_X0] = con_cells[r][c];
        }
    }
    /* cursor: draw the cell under it in reverse video (C64 block cursor);
     * during the off phase of the blink, draw it normal. Only the background
     * shows on a blank cell, so the cursor MUST change bg to be visible. */
    {
        vih_tile_cell_t cur;
        cur.raw = con_cells[con_cy][con_cx];
        map[(uint32_t)(con_cy + CON_Y0) * 64u + con_cx + CON_X0] = con_cur_on
            ? con_cell(cur.f.ch, CON_BG, CON_CUR)
            : cur.raw;
    }
}

/* Cursor motion on the linear screen position. con_putc wraps to the next row
 * immediately after column 39, so readline's "back N" must wrap back up across
 * rows the same way, or editing a line longer than one row breaks. */
static void con_cursor_back(uint16_t n) {
    uint16_t pos = (uint16_t)(con_cy * CON_COLS + con_cx);
    pos = (n > pos) ? 0u : (uint16_t)(pos - n);
    con_cy = (uint8_t)(pos / CON_COLS);
    con_cx = (uint8_t)(pos % CON_COLS);
}

static void con_cursor_fwd(uint16_t n) {
    uint16_t pos = (uint16_t)(con_cy * CON_COLS + con_cx + n);
    if (pos >= CON_ROWS * CON_COLS) {
        pos = CON_ROWS * CON_COLS - 1u;
    }
    con_cy = (uint8_t)(pos / CON_COLS);
    con_cx = (uint8_t)(pos % CON_COLS);
}

/* Erase from the cursor to the end of the SCREEN. readline sends ESC [ K
 * (erase to end of line) after a delete; since its line may wrap over several
 * rows here, clearing only the current row would leave stale characters on
 * the next one. Nothing below the REPL's input line is worth keeping. */
static void con_erase_from_cursor(void) {
    for (uint16_t pos = (uint16_t)(con_cy * CON_COLS + con_cx);
         pos < CON_ROWS * CON_COLS; pos++) {
        con_cells[pos / CON_COLS][pos % CON_COLS] = con_cell(' ', CON_FG, CON_BG);
    }
}

/* Minimal VT100 parser: just the sequences MicroPython's readline and REPL
 * emit -- ESC [ n D/C (cursor back/forward), ESC [ K (erase to end of line),
 * ESC [ n A/B (up/down), ESC [ J / ESC [ 2 J (clear), ESC [ H (home).
 * Anything else is swallowed instead of being printed as "[K" garbage. */
enum { CON_ESC_NONE, CON_ESC_ESC, CON_ESC_CSI };
static uint8_t con_esc;
static uint16_t con_esc_arg;
static uint8_t con_esc_has_arg;

static void con_csi(char final) {
    uint16_t n = con_esc_has_arg ? con_esc_arg : 1u;
    switch (final) {
        case 'D': con_cursor_back(n); break;
        case 'C': con_cursor_fwd(n); break;
        case 'A': con_cy = (n > con_cy) ? 0u : (uint8_t)(con_cy - n); break;
        case 'B':
            con_cy = (uint8_t)(((uint16_t)con_cy + n >= CON_ROWS) ? CON_ROWS - 1u : con_cy + n);
            break;
        case 'K': con_erase_from_cursor(); break;
        case 'J':
            if (con_esc_has_arg && con_esc_arg == 2u) {
                con_clear();
            } else {
                con_erase_from_cursor();
            }
            break;
        case 'H': con_cx = 0u; con_cy = 0u; break;
        default: break;
    }
}

static void con_scroll(void) {
    /* shift every row up, clear the bottom row. */
    memmove(&con_cells[0], &con_cells[1], sizeof(con_cells) - sizeof(con_cells[0]));
    for (uint8_t c = 0; c < CON_COLS; c++) {
        con_cells[CON_ROWS - 1u][c] = con_cell(' ', CON_FG, CON_BG);
    }
    con_cy = CON_ROWS - 1u;
}

static void con_clear(void) {
    for (uint8_t r = 0; r < CON_ROWS; r++) {
        for (uint8_t c = 0; c < CON_COLS; c++) {
            con_cells[r][c] = con_cell(' ', CON_FG, CON_BG);
        }
    }
    con_cx = 0u;
    con_cy = 0u;
}

static void con_putc(char ch) {
    if (con_esc == CON_ESC_ESC) {
        if (ch == '[') {
            con_esc = CON_ESC_CSI;
            con_esc_arg = 0u;
            con_esc_has_arg = 0u;
        } else {
            con_esc = CON_ESC_NONE;   /* unsupported ESC x: drop it */
        }
        return;
    }
    if (con_esc == CON_ESC_CSI) {
        if (ch >= '0' && ch <= '9') {
            if (con_esc_arg < 1000u) {
                con_esc_arg = (uint16_t)(con_esc_arg * 10u + (uint16_t)(ch - '0'));
            }
            con_esc_has_arg = 1u;
            return;
        }
        if (ch == ';' || ch == '?') {
            return;   /* multi-arg / private sequences: only the final byte matters */
        }
        con_esc = CON_ESC_NONE;
        con_csi(ch);
        con_redraw();
        return;
    }
    if (ch == '\x1B') {
        con_esc = CON_ESC_ESC;
        return;
    }
    if (ch == '\r') {
        con_cx = 0u;
        con_redraw();
        return;
    }
    if (ch == '\n') {
        con_cx = 0u;
        con_cy++;
        if (con_cy >= CON_ROWS) {
            con_scroll();
        }
        con_redraw();
        return;
    }
    if (ch == '\b') {
        /* VT100 semantics: move left only, do NOT erase. readline uses "\b"
         * to step back over text it then redraws (and ESC [ K to erase); an
         * erasing backspace wiped the characters right of the cursor. */
        con_cursor_back(1u);
        con_redraw();
        return;
    }
    if (ch == '\t') {
        con_cx = (uint8_t)(((uint16_t)con_cx + 8u) & ~7u);
        if (con_cx >= CON_COLS) {
            con_cx = CON_COLS - 1u;
        }
        con_redraw();
        return;
    }
    if ((unsigned char)ch < 0x20u) {
        return;   /* other control chars: ignore */
    }

    con_cells[con_cy][con_cx] = con_cell((uint8_t)ch, CON_FG, CON_BG);
    con_cx++;
    if (con_cx >= CON_COLS) {
        con_cx = 0u;
        con_cy++;
        if (con_cy >= CON_ROWS) {
            con_scroll();
        }
    }
    con_redraw();
}

void uart_tx_strn(const char *buf, size_t len) {
    for (size_t i = 0u; i < len; i++) {
        con_putc(buf[i]);
    }
    /* any output lights the cursor up and restarts its blink cycle */
    con_cur_on = 1u;
    con_blink_ms = (uint32_t)ticks_ms() + 300u;
}

/* ---- keyboard input -------------------------------------------------------
 * On real MEGA65 hardware (m65_keyboard.sv, native mode) the 0x190 FIFO does
 * NOT carry PS/2 set-2 scancodes: every event is code = 0x80 | key_num with
 * key_num = col*8 + row of the MEGA65 keyboard matrix (0..79, M2M scan),
 * extended=0. The PS/2 set-2 path (codes < 0x80) is kept for MiSTer builds.
 *
 * The matrix tables below are the MEGA65 KERNAL's kb_matrix (normal/shift,
 * PETSCII) converted to ASCII: unshifted letters lowercase, shifted letters
 * uppercase, US keytops (MEGA65 keyboards are US layout). Arrows are encoded
 * as 0x81..0x84 markers and expanded to ESC [ A/B/C/D for the REPL readline.
 */
#if VIXEN_LVGL
#include "vx_lvgl.h"
#endif

/* Show or hide the console (the tile layer). The LVGL build hides it while the
 * GUI is up; F1 toggles. While it is hidden and Python registered a handler
 * with vixen.lv_keys(), keys go to that handler (gui_key), else to the REPL. */
static bool console_hidden;

void uart_console_show(bool on) {
    console_hidden = !on;
    VIH_TILES_ENABLE = on ? 1u : 0u;
    /* the console's blue behind/around the text; black for the GUI, whose
     * transparent index 0 and the screen edge show these colours */
    VIH_BG_COLOR = on ? con_bg : 0u;
    VIH_BORDER_COLOR = on ? con_bg : 0u;
}

/* The LVGL palette is loaded: switch the console to the cube's C64 blues
 * (6x6x6 cube, index = r*36 + g*6 + b: blue 1,1,2 = 44, light blue 2,2,4 = 88,
 * white 215) and re-colour every cell. */
void uart_console_lvgl_palette(void) {
    con_fg = 88u;
    con_bg = 44u;
    con_curc = 215u;
    for (uint32_t r = 0u; r < CON_ROWS; r++) {
        for (uint32_t c = 0u; c < CON_COLS; c++) {
            vih_tile_cell_t cell;
            cell.raw = con_cells[r][c];
            con_cells[r][c] = con_cell(cell.f.ch, CON_FG, CON_BG);
        }
    }
    for (uint16_t i = 0u; i < VIH_TILE_MAP_CELLS; i++) {
        VIH_TILE_MAP0_BASE[i] = con_cell(' ', CON_FG, CON_BG);
    }
    con_redraw();
}

static uint8_t kbd_shift;    /* number of shift keys held (l+r) */
static uint8_t kbd_ctrl;     /* number of ctrl keys held */
static uint8_t kbd_caps;     /* caps lock: 1 = letters uppercase */
static uint8_t kbd_mega;     /* number of MEGA (C=) / ALT keys held */

/* Decoded input bytes waiting for the REPL. uart_poll() drains the 8-deep
 * hardware FIFO into this ring -- also while Python code runs (VM hook), so
 * typed-ahead keys are not lost and Ctrl-C can be seen. Power-of-two size. */
#define KBD_BUF_SIZE 64u
static uint8_t kbd_buf[KBD_BUF_SIZE];
static uint8_t kbd_buf_rd;
static uint8_t kbd_buf_wr;

/* Typematic repeat: the MEGA65 matrix only reports make/break, so a held key
 * is repeated in software after KBD_RPT_DELAY_MS, every KBD_RPT_RATE_MS. */
#define KBD_RPT_DELAY_MS 500u
#define KBD_RPT_RATE_MS  50u
static uint8_t kbd_rpt_code;     /* raw code of the held key */
static char kbd_rpt_char;        /* what it produced (char or arrow marker) */
static uint8_t kbd_rpt_active;

#if VIXEN_SOUND
/* vixen_sound.c (chip RAM): bit 0 = a SID tune plays and wants its play
 * routine called. Kept here, in the BRAM .sbss, so the test below is one
 * zero-wait load when nothing plays. */
volatile uint8_t vixen_sound_active;
void vixen_sound_poll(void);
#endif
static uint32_t kbd_rpt_next_ms;

#define K_UP  0x81u
#define K_DN  0x82u
#define K_RG  0x83u
#define K_LF  0x84u

/* PS/2 set-2 make-code tables (MiSTer builds; codes < 0x80). */
static const char K_BASE[256] = {
    ['\x15'] = 'q', ['\x1D'] = 'w', ['\x24'] = 'e', ['\x2D'] = 'r',
    ['\x2C'] = 't', ['\x35'] = 'y', ['\x3C'] = 'u', ['\x43'] = 'i',
    ['\x44'] = 'o', ['\x4D'] = 'p',
    ['\x1C'] = 'a', ['\x1B'] = 's', ['\x23'] = 'd', ['\x2B'] = 'f',
    ['\x34'] = 'g', ['\x33'] = 'h', ['\x3B'] = 'j', ['\x42'] = 'k',
    ['\x4B'] = 'l',
    ['\x1A'] = 'z', ['\x22'] = 'x', ['\x21'] = 'c', ['\x2A'] = 'v',
    ['\x32'] = 'b', ['\x31'] = 'n', ['\x3A'] = 'm',
    ['\x16'] = '1', ['\x1E'] = '2', ['\x26'] = '3', ['\x25'] = '4',
    ['\x2E'] = '5', ['\x36'] = '6', ['\x3D'] = '7', ['\x3E'] = '8',
    ['\x46'] = '9', ['\x45'] = '0',
    ['\x4E'] = '-', ['\x55'] = '=', ['\x4C'] = ';',
    ['\x41'] = ',', ['\x49'] = '.', ['\x4A'] = '/',
    ['\x54'] = '[', ['\x5B'] = ']', ['\x5D'] = '\\', ['\x0E'] = '`',
    ['\x0D'] = '\t', ['\x5A'] = '\r', ['\x66'] = '\b', ['\x29'] = ' ',
};

static const char K_SHIFT[256] = {
    ['\x16'] = '!', ['\x1E'] = '@', ['\x26'] = '#', ['\x25'] = '$',
    ['\x2E'] = '%', ['\x36'] = '^', ['\x3D'] = '&', ['\x3E'] = '*',
    ['\x46'] = '(', ['\x45'] = ')', ['\x4E'] = '_', ['\x55'] = '+',
    ['\x4C'] = ':', ['\x41'] = '<', ['\x49'] = '>', ['\x4A'] = '?',
    ['\x54'] = '{', ['\x5B'] = '}', ['\x5D'] = '|', ['\x0E'] = '~',
};

/* base (unshifted) char per MEGA65 key number; 0 = none. */
static const char M_BASE[80] = {
    [0] = '\b',            /* INST/DEL */
    [1] = '\r',            /* RETURN */
    [2] = K_RG,            /* cursor right */
    [7] = K_DN,            /* cursor down */
    [8] = '3', [9] = 'w', [10] = 'a', [11] = '4', [12] = 'z', [13] = 's', [14] = 'e',
    [16] = '5', [17] = 'r', [18] = 'd', [19] = '6', [20] = 'c', [21] = 'f', [22] = 't', [23] = 'x',
    [24] = '7', [25] = 'y', [26] = 'g', [27] = '8', [28] = 'b', [29] = 'h', [30] = 'u', [31] = 'v',
    [32] = '9', [33] = 'i', [34] = 'j', [35] = '0', [36] = 'm', [37] = 'k', [38] = 'o', [39] = 'n',
    [40] = '+', [41] = 'p', [42] = 'l', [43] = '-', [44] = '.', [45] = ':', [46] = '@', [47] = ',',
    [48] = '\\',           /* GBP key -> backslash */
    [49] = '*', [50] = ';', [53] = '=', [54] = '^', [55] = '/',
    [56] = '1', [57] = '_', [59] = '2', [60] = ' ', [62] = 'q',
    [63] = '\x03',         /* RUN/STOP -> Ctrl-C (KeyboardInterrupt) */
    [65] = '\t',           /* TAB */
    [71] = '\x1B',         /* ESC */
    [73] = K_UP,           /* cursor up (col9) */
    [74] = K_LF,           /* cursor left (col9) */
    [76] = K_DN,           /* cursor down (col9) */
    [77] = K_RG,           /* cursor right (col9) */
};

/* shifted glyph per MEGA65 key number; 0 = none. */
static const char M_SHIFT[80] = {
    [8] = '#', [11] = '$', [16] = '%', [19] = '&', [24] = '\'', [27] = '(',
    [32] = ')', [35] = '=', [40] = '?', [43] = '=', [44] = '>', [45] = '[',
    [46] = '"', [47] = '<', [50] = ']', [55] = '?', [56] = '!', [59] = '"',
};

/* Translate one matrix key number (make) to an output byte, applying
 * shift/ctrl/caps state. Returns 0 if nothing to report. */
/* MEGA (C=) or ALT held: the ASCII characters the MEGA65 keyboard has no key
 * for -- Python needs them (dicts and sets, |, ~). Placed on the keys next to
 * the related brackets: MEGA+: = {, MEGA+; = }, MEGA+pound = |, MEGA+- = ~,
 * MEGA+@ = `. */
static const char M_MEGA[80] = {
    [45] = '{', [50] = '}', [48] = '|', [43] = '~', [46] = '`',
};

static char kbd_matrix_char(uint8_t n) {
    char base = M_BASE[n];
    char out;

    if (kbd_mega > 0u && M_MEGA[n] != 0) {
        return M_MEGA[n];
    }
    if (base == 0u) {
        return 0u;
    }
    if (base >= K_UP) {
        return base;   /* arrow marker: expanded by caller */
    }
    if (base >= 'a' && base <= 'z') {
        int up = (kbd_shift > 0u) ^ (kbd_caps != 0u);
        if (kbd_ctrl > 0u) {
            /* ^A..^Z from the letter itself: the case does not matter. (This
             * subtracted 'A' from the lowercase letter, so ctrl+c gave '#'
             * instead of ^C and ctrl+d '$' instead of ^D.) */
            return (char)(base - 'a' + 1);
        }
        out = up ? (char)(base - 'a' + 'A') : base;
        return out;
    }
    if (kbd_shift > 0u && M_SHIFT[n] != 0u) {
        return M_SHIFT[n];
    }
    return base;
}

static void kbd_buf_put(uint8_t b) {
    if ((uint8_t)(kbd_buf_wr - kbd_buf_rd) < KBD_BUF_SIZE) {
        kbd_buf[kbd_buf_wr++ & (KBD_BUF_SIZE - 1u)] = b;
    }
}

/* Queue one decoded key: an arrow marker expands to its ESC [ A/B/C/D
 * sequence; the interrupt char (Ctrl-C while code runs) raises
 * KeyboardInterrupt instead of being queued. */
static void kbd_emit(char c) {
    if (c >= (char)K_UP && c <= (char)K_LF) {
        static const char arrow_final[] = { 'A', 'B', 'C', 'D' };
        kbd_buf_put('\x1B');
        kbd_buf_put('[');
        kbd_buf_put((uint8_t)arrow_final[(uint8_t)c - K_UP]);
        return;
    }
    if ((unsigned char)c == mp_interrupt_char) {
        kbd_buf_rd = kbd_buf_wr;   /* drop type-ahead, like a terminal ^C */
        mp_sched_keyboard_interrupt();
        return;
    }
    kbd_buf_put((uint8_t)c);
}

/* Keys that auto-repeat when held: printable chars, DEL, TAB and the arrows.
 * RETURN, ESC and Ctrl combinations do not (a repeating ^C or RETURN only
 * causes trouble). */
static int kbd_repeatable(char c) {
    unsigned char u = (unsigned char)c;
    return u >= 0x20u || u == '\b' || u == '\t';
}

/* Read one logical key event from the FIFO. Collapses E0 (extended) and F0
 * (break) prefixes. Returns 1 and sets out_code and out_make on a real make
 * event; returns 0 otherwise. */
static int kbd_read(uint8_t *out_code, uint8_t *out_make) {
    vih_key_t k;
    uint8_t code;
    int extended;

    k = vih_key_read();
    if (!k.f.valid) {
        return 0;
    }
    code = (uint8_t)k.f.code;
    extended = k.f.extended;
    vih_key_pop();

    if (code == 0xE0u) {
        /* extended key: the real code follows. */
        k = vih_key_read();
        if (!k.f.valid) {
            return 0;
        }
        code = (uint8_t)k.f.code;
        extended = 1;
        vih_key_pop();
    } else if (code == 0xF0u) {
        /* break of the plain (non-extended) key: the following code IS the
         * key that was released -- report it as a break event so modifier
         * releases (shift/ctrl) reach kbd_event(). */
        k = vih_key_read();
        if (!k.f.valid) {
            return 0;
        }
        code = (uint8_t)k.f.code;
        extended = 0;
        vih_key_pop();
        *out_code = code;
        *out_make = 0u;
        return 1;
    }

    (void)extended;
    *out_code = code;
    *out_make = k.f.pressed ? 1u : 0u;
    return 1;
}

/* Feed one raw make code (with its make/break flag) into the modifier/char
 * logic. Returns an ASCII byte, 0x81..0x84 for an arrow, or 0 if nothing to
 * report. Handles BOTH code spaces: MEGA65 matrix (bit7 set) and PS/2 set-2
 * (< 0x80, MiSTer builds). */
static char kbd_event(uint8_t code, uint8_t make) {
    if (code >= 0x80u) {
        /* MEGA65 matrix native event. Key numbers per M2M framework
         * (MiSTer2MEGA65 CORE/vhdl/keyboard.vhd): 0=INS/DEL, 1=RETURN,
         * 2=cursor right, 7=cursor down, 15/52=SHIFT L/R, 51=CLR/HOME,
         * 58=CTRL, 61=C=, 63=RUN/STOP, 64=NO SCROLL, 66=ALT, 71=ESC,
         * 72=CAPS LOCK, 73=cursor up, 74=cursor left, 75=RESTORE. */
        uint8_t n = (uint8_t)(code & 0x7Fu);
        switch (n) {
            case 15: case 52:             /* shift L / R */
                if (make) { kbd_shift++; } else if (kbd_shift > 0u) { kbd_shift--; }
                return 0u;
            case 58:                      /* ctrl */
                if (make) { kbd_ctrl++; } else if (kbd_ctrl > 0u) { kbd_ctrl--; }
                return 0u;
            case 72:                      /* caps lock: toggle on make */
                if (make) { kbd_caps ^= 1u; }
                return 0u;
            case 61: case 66:             /* MEGA (C=) / ALT: symbol modifier */
                if (make) { kbd_mega++; } else if (kbd_mega > 0u) { kbd_mega--; }
                return 0u;
            case 64:                      /* NO SCROLL */
                return 0u;
            default:
                if (!make) {
                    return 0u;
                }
                return kbd_matrix_char(n);
        }
    }

    /* PS/2 set-2 (MiSTer). */
    if (code == 0x12u || code == 0x59u) {   /* left / right shift */
        if (make) { kbd_shift++; } else if (kbd_shift > 0u) { kbd_shift--; }
        return 0u;
    }
    if (code == 0x14u || code == 0x1Cu) {   /* left / right ctrl */
        if (make) { kbd_ctrl++; } else if (kbd_ctrl > 0u) { kbd_ctrl--; }
        return 0u;
    }
    if (code == 0x58u) {                    /* caps lock: toggle on make */
        if (make) { kbd_caps ^= 1u; }
        return 0u;
    }
    if (code == 0x76u) { return '\x1B'; }   /* ESC */

    {
        char base = K_BASE[code];
        char out;
        if (base == 0u) {
            return 0u;   /* modifier or unknown */
        }
        if (base >= 'a' && base <= 'z') {
            int up = (kbd_shift > 0u) ^ (kbd_caps != 0u);
            if (kbd_ctrl > 0u) {
                return (char)(base - 'a' + 1);   /* ^A..^Z, as above */
            }
            out = up ? (char)(base - 'a' + 'A') : base;
            return out;
        }
        if (kbd_shift > 0u && K_SHIFT[code] != 0u) {
            return K_SHIFT[code];
        }
        return base;
    }
}

/* Drain the hardware key FIFO into kbd_buf, run the typematic repeat and the
 * cursor blink. Cheap when idle (a couple of register reads); called from the
 * REPL's input wait and periodically from the VM (MICROPY_VM_HOOK_POLL). */
#if VIXEN_LVGL
/* GUI keys: while the GUI is shown and Python registered vixen.lv_keys(cb),
 * keys go to cb (as LVGL key codes) instead of the REPL. Ctrl-C / RUN/STOP
 * (^C) always go to the REPL so a running program can still be stopped. */
static bool gui_key(char c) {
    uint32_t key;

    if (!console_hidden || c == '\x03' || !vx_lvgl_mp_key_wanted()) {
        return false;
    }
    switch ((unsigned char)c) {
        case '\t':  key = kbd_shift ? 11u : 9u; break;   /* LV_KEY_PREV / NEXT */
        case K_UP:  key = 17u; break;                     /* LV_KEY_UP */
        case K_DN:  key = 18u; break;                     /* LV_KEY_DOWN */
        case K_RG:  key = 19u; break;                     /* LV_KEY_RIGHT */
        case K_LF:  key = 20u; break;                     /* LV_KEY_LEFT */
        case '\r':  key = 10u; break;                     /* LV_KEY_ENTER */
        default:    key = (unsigned char)c; break;        /* ASCII, \b = BACKSPACE, ESC */
    }
    vx_lvgl_mp_key(key);
    return true;
}
#else
#define gui_key(c) false
#endif

void vixen_entropy_add(uint32_t v);

void uart_poll(void) {
    uint8_t code;
    uint8_t make;
    uint32_t now = (uint32_t)ticks_ms();

    while (kbd_read(&code, &make)) {
        /* key timing (make and break, to the microsecond) feeds os.urandom */
        vixen_entropy_add((uint32_t)code | ((uint32_t)make << 8));
        /* Break events MUST reach kbd_event(): it decrements the shift/ctrl
         * counters there. Dropping them leaves the modifiers stuck on. */
        char c = kbd_event(code, make);
        if (!make) {
            if (kbd_rpt_active && code == kbd_rpt_code) {
                kbd_rpt_active = 0u;
            }
            continue;
        }
        #if VIXEN_LVGL
        if (code == (0x80u | 4u)) {     /* F1: GUI <-> console */
            uart_console_show(console_hidden);
            continue;
        }
        #endif
        if (c == 0) {
            continue;   /* modifier / unmapped key: keeps any repeat going */
        }
        if (!gui_key(c)) {
            kbd_emit(c);
        }
        kbd_rpt_active = (uint8_t)kbd_repeatable(c);
        kbd_rpt_code = code;
        kbd_rpt_char = c;
        kbd_rpt_next_ms = now + KBD_RPT_DELAY_MS;
    }

    if (kbd_rpt_active && (uint32_t)(now - kbd_rpt_next_ms) < 0x80000000u) {
        if (!gui_key(kbd_rpt_char)) {
            kbd_emit(kbd_rpt_char);
        }
        kbd_rpt_next_ms = now + KBD_RPT_RATE_MS;
    }

    if ((uint32_t)(now - con_blink_ms) < 0x80000000u) {
        con_blink_ms = now + 300u;
        con_cur_on ^= 1u;
        con_redraw();
    }

    #if VIXEN_LVGL
    vx_lvgl_mp_poll(now);
    #endif

    #if VIXEN_SOUND
    /* Before the Ethernet pump, which stays the tail call: a hook in tail
     * position made gcc duplicate the whole epilogue (68 bytes of BRAM). */
    if (vixen_sound_active) {
        vixen_sound_poll();
    }
    #endif

    #if MICROPY_PY_LWIP
    /* Ethernet: the core holds four frames, so the stack is pumped from here
     * too -- the REPL's input wait, time.sleep() and the VM hook. */
    vixen_eth_poll();
    #endif
}

int uart_rx_chr(void) {
    if (kbd_buf_rd == kbd_buf_wr) {
        uart_poll();
        if (kbd_buf_rd == kbd_buf_wr) {
            return UART_RX_NO_CHAR;
        }
    }
    return kbd_buf[kbd_buf_rd++ & (KBD_BUF_SIZE - 1u)];
}

int uart_rx_any(void) {
    if (kbd_buf_rd == kbd_buf_wr) {
        uart_poll();
    }
    return kbd_buf_rd != kbd_buf_wr;
}

/* Text-layer scroll in pixels (0..511 each); see the geometry notes above. */
void uart_set_tile_scroll(uint32_t x, uint32_t y) {
    vih_tile_scroll_t ts;
    ts.raw = 0u;
    ts.f.x = x;
    ts.f.y = y;
    VIH_TILE_SCROLL = ts.raw;
}

void uart_init(void) {
    vih_init_registers();

    /* Clear ctrl_word: tile_reload is LEVEL-triggered -- the video reader
     * re-fires a full tile-map reload from the KERNAL's source every poll it
     * sees the bit high. The KERNAL's splash leaves it set and a normal app
     * never clears it (vih_init_registers deliberately does not touch
     * ctrl_word), so it would keep clobbering our tile-map writes. */
    VIH_CTRL_WORD = 0u;

    /* Text-only console: tile layer on, framebuffer off. */
    VIH_TILES_ENABLE = 1u;
    VIH_GFX_ENABLE = 0u;
    VIH_BORDER_COLOR = CON_BG;
    VIH_BORDER_DIMS = 0u;          /* no border: the console fills the screen */
    VIH_BG_COLOR = CON_BG;
    uart_set_tile_scroll(TILE_SCROLL_X, TILE_SCROLL_Y);

    kbd_shift = 0u;
    kbd_ctrl = 0u;
    kbd_mega = 0u;
    kbd_rpt_active = 0u;
    kbd_buf_rd = kbd_buf_wr = 0u;
    con_cur_on = 1u;
    con_blink_ms = (uint32_t)ticks_ms() + 300u;
    con_clear();

    /* Wipe the WHOLE 64x64 tile map, not just the visible 48x28 window: with a
     * tile scroll other than 0,0 cells beyond it come into view. */
    for (uint16_t i = 0u; i < VIH_TILE_MAP_CELLS; i++) {
        VIH_TILE_MAP0_BASE[i] = con_cell(' ', CON_FG, CON_BG);
    }
    con_redraw();
}

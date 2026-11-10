/* SPDX-License-Identifier: MIT
 * Copyright (c) 2025-2026 Robert S. (aka Robmaster) */
/*
 * The `sound` module: WAV playback on the PCM engine and a PSID player on
 * the SID.
 *
 *   sound.wav.play("file.wav", loop=False)    sound.wav.stop()
 *   sound.sid.play("file.sid", song=0)         sound.sid.stop()
 *   sound.wav.playing() / sound.sid.playing()
 *
 * play() also takes a bytes-like object instead of a file name (a file read
 * into memory, a download).
 *
 * WHERE IT LIVES. Everything here -- code, constants and state -- is in chip
 * RAM, none of it in the BRAM: the object goes to .chiptext (vixen_net.ld /
 * vixen_lvgl.ld) and is compiled with -msmall-data-limit=0, so not even a
 * small global lands in the gp-relative .sbss. The BRAM cost of the module is
 * its qstrs, its entry in the module table and the hook in uart_poll().
 * The audio data uses the chip-RAM window SND_BASE..SND_END, which no build
 * uses for anything else (see the linker scripts).
 *
 * WAV. The file is loaded completely into chip RAM, then the PCM engine plays
 * it by itself: no CPU time at all while it plays. 8/16/24/32-bit integer
 * PCM, mono or stereo, any rate. The engine plays at most 48 kHz (one input
 * sample per mix tick), so a faster file is decimated by averaging. Stereo
 * uses voices 0 (left) and 1 (right) with the engine's 75/25 panning -- the
 * hardware has no hard left/right. Samples above 8 bit are stored as 16 bit,
 * or as 8 bit when the 16-bit copy would not fit the window (~4.4 MB: about
 * 47 s of 48 kHz 16-bit mono, twice that in 8 bit).
 *
 * SID. A .sid file (PSID or RSID) carries 6502 code: an init and a play
 * routine or interrupt handler. vx_sidplay.h runs them on vx6502.h against a
 * 64 KB C64 memory image in chip RAM (what of the C64 is modelled: see
 * there); every write to $D400-$D7FF goes to the VIXEN SID. The calls are
 * due 50 times a second, or at the raster/CIA rate the tune programs; there
 * are no interrupts in this port, so uart_poll() -- the VM hook,
 * time.sleep(), the REPL's input wait -- calls vixen_sound_poll() while a
 * tune plays. Nothing playing: uart_poll() tests one byte and goes on. While
 * a tune plays it costs roughly 3-5 % of the CPU (estimate from instruction
 * counts, not measured on the hardware). C code that blocks for longer than
 * a frame (a large file read, an LVGL redraw) delays the SID writes and the
 * tune stumbles briefly; it never runs faster than it should. The writes are
 * time-stamped with their C64 cycle and given out at that time (see
 * vx_sidplay.h, TIMING), so they are spread over the frame as on a C64 --
 * including digis through the volume register -- at the resolution of how
 * often uart_poll() runs.
 *
 * The VIXEN SID is clocked at exactly 1 MHz, a PAL C64 at 0.985 MHz: the
 * oscillator frequencies are scaled by the tune's clock (PAL/NTSC) so the
 * pitch is right. The SID's register file is replayed into the chip every
 * 290 ns, so a gate bit cleared and set again within a microsecond -- normal
 * for code running 20x faster than a C64 -- could go unseen; a control
 * register write therefore waits until 2 us after the previous one.
 */

#include <string.h>

#include "py/runtime.h"
#include "py/stream.h"
#include "py/builtin.h"
#include "py/mphal.h"

#include "vih_regs.h"

#if VIXEN_SOUND

extern uint64_t ticks_raster_lines(void);
extern volatile uint8_t vixen_sound_active;   /* uart.c (BRAM): bit 0 = SID tune plays */

/* ---- chip-RAM window (byte offsets from 0x40000000) ------------------- */

#define SND_BASE        0x00380000u
#define SND_END         0x007F0000u
#define SID_RAM_OFF     SND_BASE                /* 64 KB C64 RAM */
#define SID_IO_OFF      (SND_BASE + 0x10000u)   /* 4 KB I/O register shadow */
#define WAV_OFF         (SND_BASE + 0x11000u)
#define WAV_MAX         (SND_END - WAV_OFF)
#define CHIP(off)       ((uint8_t *)(uintptr_t)(VIH_SDRAM_CACHED_BASE + (off)))

/* PCM mix tick: pcm_audio.sv divides the 100 MHz clock by 100e6/48000 = 2083. */
#define PCM_CLK         100000000u
#define PCM_DIV         2083u
#define PCM_MAX_RATE    (PCM_CLK / PCM_DIV)     /* 48007 Hz */

/* ---- SID output, then the player ---------------------------------------- */

static struct {
    uint32_t ctrl_hpos;     /* raster HPOS/VPOS of the last control write */
    uint32_t ctrl_vpos;
    uint8_t sid_used;       /* the SID was written: the first write takes it over */
    /* wave */
    uint8_t w_on;
    uint8_t w_loop;
    uint8_t w_trig;
    uint32_t w_start_ms;
    uint32_t w_dur_ms;
    vih_pcm_ctrl_t pcm;
    uint8_t scratch[4096];
} snd;

static void sidp_out(unsigned reg, unsigned v) {
    vih_sid_t s = { .raw = 0 };
    s.f.reg = reg;
    s.f.value = v;
    VIH_SID = s.raw;
}

/* A control register write waits until 2 us (16 HPOS pixels of 128 ns)
 * after the previous one, so the SID sees every gate/test edge. */
static void sidp_out_ctrl(unsigned reg, unsigned v) {
    for (;;) {
        uint32_t vp = VIH_RASTER_VPOS;
        uint32_t h = VIH_RASTER_HPOS;
        if (vp != snd.ctrl_vpos) {
            if (vp != (snd.ctrl_vpos + 1u) % 312u || h + 500u - snd.ctrl_hpos >= 16u) {
                break;
            }
        } else if (h - snd.ctrl_hpos >= 16u || h < snd.ctrl_hpos) {
            break;
        }
    }
    sidp_out(reg, v);
    snd.ctrl_vpos = VIH_RASTER_VPOS;
    snd.ctrl_hpos = VIH_RASTER_HPOS;
}

#include "vx_sidplay.h"

/* The player's clock: raster lines << 8 plus the beam's position in the
 * line (HPOS 0..499 at 128 ns -> 0..255), i.e. 0.25 us units. If the line
 * changed between the two HPOS reads, the line count is read again so line
 * and position belong together. */
static uint32_t snd_now(void) {
    uint32_t h1 = VIH_RASTER_HPOS;
    uint32_t l = (uint32_t)ticks_raster_lines();
    uint32_t h2 = VIH_RASTER_HPOS;
    if (h2 < h1) {
        l = (uint32_t)ticks_raster_lines();
        h1 = h2;
    }
    if (h1 > 499u) {
        h1 = 499u;
    }
    return (l << 8) + ((h1 * 131u) >> 8);
}

static void sid_stop_internal(void) {
    vixen_sound_active &= (uint8_t)~1u;
    if (snd.sid_used) {
        sidp_silence();
    }
}

/* Called from uart_poll() while a tune plays. Never raises. */
void vixen_sound_poll(void) {
    if (sidp_poll(snd_now())) {
        sid_stop_internal();                    /* the tune is stuck */
    }
}

/* ---- reading a file or a buffer ----------------------------------------- */

typedef struct {
    mp_obj_t file;
    const uint8_t *buf;
    size_t len;
    size_t pos;
} snd_src_t;

static void src_open(snd_src_t *s, mp_obj_t src) {
    s->file = MP_OBJ_NULL;
    s->buf = NULL;
    s->len = s->pos = 0;
    if (mp_obj_is_str(src)) {
        mp_obj_t args[2] = { src, MP_OBJ_NEW_QSTR(MP_QSTR_rb) };
        s->file = mp_builtin_open(2, args, (mp_map_t *)&mp_const_empty_map);
    } else {
        mp_buffer_info_t bi;
        mp_get_buffer_raise(src, &bi, MP_BUFFER_READ);
        s->buf = bi.buf;
        s->len = bi.len;
    }
}

static size_t src_read(snd_src_t *s, void *dst, size_t n) {
    if (s->file == MP_OBJ_NULL) {
        size_t k = s->len - s->pos;
        if (k > n) {
            k = n;
        }
        memcpy(dst, s->buf + s->pos, k);
        s->pos += k;
        return k;
    }
    int err;
    mp_uint_t got = mp_stream_rw(s->file, dst, n, &err, MP_STREAM_RW_READ);
    if (err) {
        mp_raise_OSError(err);
    }
    return got;
}

static void src_skip(snd_src_t *s, uint32_t n) {
    while (n) {
        size_t k = n < 256u ? n : 256u;
        uint8_t tmp[256];
        if (src_read(s, tmp, k) != k) {
            return;
        }
        n -= k;
    }
}

static void src_close(snd_src_t *s) {
    if (s->file != MP_OBJ_NULL) {
        mp_obj_t f = s->file;
        s->file = MP_OBJ_NULL;
        mp_stream_close(f);
    }
}

static uint32_t le16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static uint32_t le32(const uint8_t *p) {
    return le16(p) | (le16(p + 2) << 16);
}

static MP_NORETURN void bad_file(void) {
    mp_raise_ValueError(MP_ERROR_TEXT("unsupported file"));
}

/* ---- PCM ---------------------------------------------------------------- */

static void pcm_commit(void) {
    snd.pcm.magic = VIH_PCM_MAGIC;
    vih_pcm_commit(&snd.pcm);
}

static void wave_stop_internal(void) {
    snd.w_on = 0;
    snd.pcm.ch[0].flags.f.en = 0;
    snd.pcm.ch[1].flags.f.en = 0;
    pcm_commit();
}

typedef struct {
    uint32_t channels, rate, bits, frames;
} wav_fmt_t;

static void wave_parse(snd_src_t *s, wav_fmt_t *w) {
    uint8_t h[40];
    if (src_read(s, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        bad_file();
    }
    int have_fmt = 0;
    for (;;) {
        if (src_read(s, h, 8) != 8) {
            bad_file();                         /* no data chunk */
        }
        uint32_t size = le32(h + 4);
        if (!memcmp(h, "data", 4)) {
            break;
        }
        if (!memcmp(h, "fmt ", 4) && size >= 16u) {
            uint32_t k = size < sizeof(h) ? size : sizeof(h);
            if (src_read(s, h, k) != k) {
                bad_file();
            }
            uint32_t tag = le16(h);
            if (tag == 0xFFFEu && k >= 26u) {
                tag = le16(h + 24);             /* WAVE_FORMAT_EXTENSIBLE sub-format */
            }
            w->channels = le16(h + 2);
            w->rate = le32(h + 4);
            w->bits = le16(h + 14);
            if (tag != 1u || w->channels < 1u || w->channels > 2u || w->rate < 1000u
                || w->rate > 384000u || (w->bits != 8u && w->bits != 16u && w->bits != 24u && w->bits != 32u)) {
                bad_file();
            }
            have_fmt = 1;
            size -= k;
        }
        src_skip(s, size + (size & 1u));
    }
    if (!have_fmt) {
        bad_file();
    }
    uint32_t size = le32(h + 4);
    w->frames = size / (w->channels * (w->bits / 8u));   /* 0/garbage: until EOF */
    if (w->frames == 0u) {
        w->frames = 0xFFFFFFFFu;
    }
}

static void wave_load(snd_src_t *s, mp_int_t loop) {
    wav_fmt_t w;
    wave_parse(s, &w);

    uint32_t bps = w.bits / 8u, frame = w.channels * bps;
    uint32_t decim = (w.rate + PCM_MAX_RATE - 1u) / PCM_MAX_RATE;
    uint32_t rate = w.rate / decim;
    int known = w.frames != 0xFFFFFFFFu;
    uint32_t want = w.frames / decim;           /* output samples per channel */
    uint32_t cap = (WAV_MAX / w.channels) & ~3u;
    int out16 = w.bits > 8u;
    if (known && out16 && want > cap / 2u) {
        out16 = 0;                              /* 16 bit does not fit: store 8 bit */
    }
    uint32_t max = out16 ? cap / 2u : cap;
    if (want > max) {
        if (known) {
            mp_raise_ValueError(MP_ERROR_TEXT("WAV too long"));
        }
        want = max;                             /* unknown length: what fits */
    }

    uint8_t *dst[2] = { CHIP(WAV_OFF), CHIP(WAV_OFF + cap) };
    uint32_t n = 0;                             /* samples per channel written */
    uint32_t chunk = (sizeof(snd.scratch) / (frame * decim)) * decim;
    int32_t acc[2];
    int8_t pend[2] = { 0, 0 };
    uint32_t in_left = w.frames;
    while (n < want) {
        uint32_t fr = chunk;
        if (fr > in_left) {
            fr = in_left;
        }
        size_t got = src_read(s, snd.scratch, fr * frame) / frame;
        if (got < decim) {
            break;                              /* EOF */
        }
        in_left -= got;
        const uint8_t *p = snd.scratch;
        for (uint32_t i = 0; i + decim <= got && n < want; i += decim) {
            for (uint32_t c = 0; c < w.channels; c++) {
                acc[c] = 0;
            }
            for (uint32_t d = 0; d < decim; d++) {
                for (uint32_t c = 0; c < w.channels; c++) {
                    int32_t v;
                    if (bps == 1u) {
                        v = ((int32_t)p[0] - 128) << 8;
                    } else {
                        v = (int16_t)(p[bps - 2u] | (p[bps - 1u] << 8));   /* top 16 bits */
                    }
                    acc[c] += v;
                    p += bps;
                }
            }
            for (uint32_t c = 0; c < w.channels; c++) {
                int32_t v = acc[c] / (int32_t)decim;
                if (out16) {
                    ((int16_t *)dst[c])[n] = (int16_t)v;
                } else if (n & 1u) {
                    /* two 8-bit samples per halfword, first sample low */
                    ((uint16_t *)dst[c])[n >> 1] = (uint16_t)((uint8_t)pend[c] | ((uint8_t)(v >> 8) << 8));
                } else {
                    pend[c] = (int8_t)(v >> 8);
                }
            }
            n++;
        }
        if (in_left == 0u) {
            break;
        }
    }
    if (!out16) {
        n &= ~1u;                               /* the engine reads whole words */
    }
    if (n < 2u) {
        bad_file();
    }

    /* phase increment = rate / mix rate, Q16 = rate * 65536 * 2083 / 1e8
     * (89465 = that factor in Q16; no 64-bit division -> no libgcc in BRAM) */
    uint32_t pitch = (uint32_t)(((uint64_t)rate * 89465u) >> 16);
    for (uint32_t c = 0; c < 2u; c++) {
        vih_pcm_ch_t *ch = &snd.pcm.ch[c];
        ch->base = (c && w.channels == 2u) ? WAV_OFF + cap : WAV_OFF;
        ch->len = n;
        ch->pitch = pitch;
        ch->flags.raw = 0;
        ch->flags.f.vol = 255u;
        ch->flags.f.en = c < w.channels;
        ch->flags.f.is16 = (uint32_t)out16;
        ch->flags.f.loop = loop ? 1u : 0u;
    }
    snd.pcm.header.raw = 0;
    snd.pcm.header.f.master_vol = 255u;
    snd.pcm.header.f.stereo = w.channels == 2u;
    snd.w_trig ^= 3u;
    snd.pcm.header.f.trig = snd.w_trig;
    VIH_AUDIO_ENABLE = 1u;
    pcm_commit();
    snd.w_on = 1;
    snd.w_loop = loop ? 1 : 0;
    snd.w_start_ms = (uint32_t)mp_hal_ticks_ms();
    snd.w_dur_ms = n / rate * 1000u + (n % rate) * 1000u / rate + 1u;
}

/* ---- PSID / RSID -------------------------------------------------------- */

static mp_int_t sid_load(snd_src_t *s, mp_int_t song) {
    uint8_t h[0x7C];
    sidp_hdr_t hd;
    if (src_read(s, h, 0x76) != 0x76) {
        bad_file();
    }
    unsigned hl = sidp_hdr_len(h);
    if ((hl > 0x76u && src_read(s, h + 0x76, hl - 0x76u) != hl - 0x76u) || sidp_parse(&hd, h)) {
        bad_file();
    }
    src_skip(s, hd.off - hl);
    uint32_t load = hd.load;
    if (load == 0u) {
        uint8_t la[2];
        if (src_read(s, la, 2) != 2) {
            bad_file();
        }
        load = le16(la);
    }
    uint8_t *ram = CHIP(SID_RAM_OFF);
    memset(ram, 0, 0x10000u);
    size_t len = src_read(s, ram + load, 0x10000u - load);
    if (len == 0u) {
        bad_file();
    }
    int r = sidp_setup(&hd, ram, CHIP(SID_IO_OFF), load, (uint32_t)len, (int)song);
    if (r < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("no such song"));
    }
    return r;
}

static void sid_start(void) {
    VIH_AUDIO_ENABLE = 1u;
    snd.sid_used = 1;
    snd.ctrl_vpos = VIH_RASTER_VPOS;
    snd.ctrl_hpos = VIH_RASTER_HPOS;
    sidp_start(snd_now());
    vixen_sound_active |= 1u;
}

/* ---- Python bindings ---------------------------------------------------- */

static mp_obj_t snd_wave_play(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_src, ARG_loop };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_loop, MP_ARG_BOOL, {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    wave_stop_internal();                       /* the engine must not read what we overwrite */
    snd_src_t s;
    src_open(&s, args[ARG_src].u_obj);
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        wave_load(&s, args[ARG_loop].u_bool);
        nlr_pop();
    } else {
        src_close(&s);
        nlr_jump(nlr.ret_val);
    }
    src_close(&s);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(snd_wave_play_obj, 1, snd_wave_play);

static mp_obj_t snd_wave_stop(void) {
    wave_stop_internal();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(snd_wave_stop_obj, snd_wave_stop);

static mp_obj_t snd_wave_playing(void) {
    if (snd.w_on && !snd.w_loop && (uint32_t)mp_hal_ticks_ms() - snd.w_start_ms >= snd.w_dur_ms) {
        snd.w_on = 0;
    }
    return mp_obj_new_bool(snd.w_on);
}
static MP_DEFINE_CONST_FUN_OBJ_0(snd_wave_playing_obj, snd_wave_playing);

static mp_obj_t snd_sid_play(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_src, ARG_song };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_song, MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    sid_stop_internal();
    snd_src_t s;
    src_open(&s, args[ARG_src].u_obj);
    mp_int_t song;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        song = sid_load(&s, args[ARG_song].u_int);
        nlr_pop();
    } else {
        src_close(&s);
        nlr_jump(nlr.ret_val);
    }
    src_close(&s);
    (void)song;
    sid_start();                                /* runs the tune's init */
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(snd_sid_play_obj, 1, snd_sid_play);

static mp_obj_t snd_sid_stop(void) {
    sid_stop_internal();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(snd_sid_stop_obj, snd_sid_stop);

static mp_obj_t snd_sid_playing(void) {
    return mp_obj_new_bool(vixen_sound_active & 1u);
}
static MP_DEFINE_CONST_FUN_OBJ_0(snd_sid_playing_obj, snd_sid_playing);

/* Soft reset (main.c): silence what we used, touch nothing else. The
 * chip-RAM window holds no Python objects. */
void vixen_sound_reset(void) {
    sid_stop_internal();
    if (snd.pcm.magic == VIH_PCM_MAGIC) {
        wave_stop_internal();                   /* only if we ever took the PCM engine */
    }
}

static const mp_rom_map_elem_t snd_wave_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_wav) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&snd_wave_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&snd_wave_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_playing), MP_ROM_PTR(&snd_wave_playing_obj) },
};
static MP_DEFINE_CONST_DICT(snd_wave_globals, snd_wave_globals_table);
static const mp_obj_module_t snd_wave_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&snd_wave_globals,
};

static const mp_rom_map_elem_t snd_sid_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_sid) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&snd_sid_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&snd_sid_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_playing), MP_ROM_PTR(&snd_sid_playing_obj) },
};
static MP_DEFINE_CONST_DICT(snd_sid_globals, snd_sid_globals_table);
static const mp_obj_module_t snd_sid_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&snd_sid_globals,
};

static const mp_rom_map_elem_t snd_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_sound) },
    { MP_ROM_QSTR(MP_QSTR_wav), MP_ROM_PTR(&snd_wave_module) },
    { MP_ROM_QSTR(MP_QSTR_sid), MP_ROM_PTR(&snd_sid_module) },
};
static MP_DEFINE_CONST_DICT(snd_globals, snd_globals_table);

const mp_obj_module_t mp_module_sound = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&snd_globals,
};

MP_REGISTER_MODULE(MP_QSTR_sound, mp_module_sound);

#endif

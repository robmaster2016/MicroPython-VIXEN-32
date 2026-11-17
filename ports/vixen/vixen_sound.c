/* SPDX-License-Identifier: MIT
 * Copyright (c) 2025-2026 Robert S. (aka Robmaster) */
/*
 * The `sound` module: WAV playback on the PCM engine and a PSID player on
 * the SID.
 *
 *   sound.wav.play("file.wav", loop=False, stream=None)    sound.wav.stop()
 *   sound.qoa.play("file.qoa", loop=False, stream=None)    sound.qoa.stop()
 *   sound.sid.play("file.sid", song=0)         sound.sid.stop()
 *   sound.wav.playing() / sound.sid.playing()
 *   sound.wav.pause() / .resume() / .paused()   (the same for qoa and sid)
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
 * 47 s of 48 kHz 16-bit mono, twice that in 8 bit). A longer file, one of
 * unknown length, or any file with stream=True streams through the window
 * as a ring instead (see "streaming" below).
 *
 * IMA ADPCM (format tag 0x11, 4 bit, as written by `ffmpeg -c:a
 * adpcm_ima_wav` or `sox -e ima-adpcm`) is decoded while loading, to 16 bit
 * like any other file: the file is 4x smaller than 16-bit PCM, the time that
 * fits the window is the same. Each block starts with a 4-byte header per
 * channel (first sample, step index); then, per channel in turn, 4 bytes = 8
 * samples, low nibble first.
 *
 * QOA (sound.qoa, "Quite OK Audio", see the QOA section) goes the same way:
 * 3.2 bits per sample, decoded to 16 bit while loading or streaming. It
 * shares the engine with sound.wav, so playing one stops the other.
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
extern volatile uint8_t vixen_sound_active;   /* uart.c (BRAM): bit 0 = SID tune plays,
                                                 * bit 1 = a WAV streams through the ring */

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
    uint8_t w_paused;       /* wav.pause(): engine at pitch 0, volume 0 */
    uint8_t s_paused;       /* sid.pause(): player not polled, SID volume 0 */
    uint32_t w_start_ms;
    uint32_t w_dur_ms;
    uint32_t w_pause_ms;    /* when wav.pause() was called */
    uint32_t w_pitch;       /* the engine's pitch before the pause */
    uint32_t s_pause_t;     /* snd_now() at sid.pause() */
    vih_pcm_ctrl_t pcm;
    uint8_t raw[4096];      /* file bytes: PCM frames or one ADPCM block */
    int16_t dec[2048];      /* decoded frames, channels interleaved */
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
    snd.s_paused = 0;
    if (snd.sid_used) {
        sidp_silence();
    }
}

static void st_poll(void);

/* Called from uart_poll() while a tune plays or a WAV streams. Never raises. */
void vixen_sound_poll(void) {
    if ((vixen_sound_active & 1u) && sidp_poll(snd_now())) {
        sid_stop_internal();                    /* the tune is stuck */
    }
    if (vixen_sound_active & 2u) {
        st_poll();
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
    s->pos += got;
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

/* Back to byte offset pos (from the start of the file or buffer). */
static void src_seek(snd_src_t *s, size_t pos) {
    if (s->file != MP_OBJ_NULL) {
        int err;
        if (mp_stream_seek(s->file, (mp_off_t)pos, MP_SEEK_SET, &err) == (mp_off_t)-1) {
            mp_raise_OSError(err);
        }
    }
    s->pos = pos;
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
    snd.w_paused = 0;
    snd.pcm.ch[0].flags.f.en = 0;
    snd.pcm.ch[1].flags.f.en = 0;
    pcm_commit();
}

typedef struct {
    uint32_t channels, rate, bits, frames;
    uint32_t codec;         /* CODEC_PCM, CODEC_ADPCM or CODEC_QOA */
    uint32_t align, spb;    /* ADPCM: block bytes, samples per channel per block */
    uint32_t data;          /* bytes in the data chunk, 0xFFFFFFFF: until EOF */
} wav_fmt_t;

#define WAVE_UNKNOWN    0xFFFFFFFFu
#define CODEC_PCM       0u
#define CODEC_ADPCM     1u      /* IMA ADPCM in a WAV */
#define CODEC_QOA       2u      /* a .qoa file (sound.qoa) */

static uint32_t be16(const uint8_t *p) {
    return ((uint32_t)p[0] << 8) | p[1];
}
static uint32_t be32(const uint8_t *p) {
    return (be16(p) << 16) | be16(p + 2);
}

static void wave_parse(snd_src_t *s, wav_fmt_t *w) {
    uint8_t h[40];
    if (src_read(s, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        bad_file();
    }
    int have_fmt = 0;
    uint32_t fact = WAVE_UNKNOWN;
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
            w->align = le16(h + 12);
            w->bits = le16(h + 14);
            w->codec = tag == 0x11u ? CODEC_ADPCM : CODEC_PCM;
            if (w->channels < 1u || w->channels > 2u || w->rate < 1000u || w->rate > 384000u) {
                bad_file();
            }
            if (w->codec == CODEC_ADPCM) {
                /* header 4 bytes per channel, then whole 4-byte groups per channel */
                uint32_t hd = 4u * w->channels;
                if (w->bits != 4u || w->align <= hd || w->align > sizeof(snd.raw) || (w->align - hd) % hd) {
                    bad_file();
                }
                w->spb = (w->align - hd) * 2u / w->channels + 1u;
                if (k >= 20u && le16(h + 16) >= 2u && le16(h + 18) != w->spb) {
                    bad_file();                 /* cbSize/wSamplesPerBlock disagree */
                }
            } else if (tag != 1u || (w->bits != 8u && w->bits != 16u && w->bits != 24u && w->bits != 32u)) {
                bad_file();
            }
            have_fmt = 1;
            size -= k;
        } else if (!memcmp(h, "fact", 4) && size >= 4u) {
            if (src_read(s, h, 4) != 4) {
                bad_file();
            }
            fact = le32(h);                     /* samples per channel */
            size -= 4u;
        }
        src_skip(s, size + (size & 1u));
    }
    if (!have_fmt) {
        bad_file();
    }
    uint32_t size = le32(h + 4);
    w->data = size ? size : WAVE_UNKNOWN;       /* 0: until EOF */
    if (w->codec == CODEC_ADPCM) {
        uint32_t frames = WAVE_UNKNOWN;
        if (size) {
            uint32_t hd = 4u * w->channels, rest = size % w->align;
            frames = size / w->align * w->spb;
            if (rest >= hd) {
                frames += (rest - hd) / hd * 8u + 1u;   /* a short last block */
            }
        }
        if (fact != WAVE_UNKNOWN && fact < frames) {
            frames = fact;                      /* the last block may be padded */
        }
        w->frames = frames;
    } else {
        w->frames = size / (w->channels * (w->bits / 8u));   /* garbage: until EOF */
    }
    if (w->frames == 0u) {
        w->frames = WAVE_UNKNOWN;
    }
}

/* A .qoa file: the file header and a peek at the first frame's header, then
 * back to the first frame (the decoder reads each frame header itself). */
static void qoa_parse(snd_src_t *s, wav_fmt_t *w) {
    uint8_t h[16];
    if (src_read(s, h, 16) != 16 || memcmp(h, "qoaf", 4)) {
        bad_file();
    }
    w->frames = be32(h + 4);                    /* samples per channel */
    w->channels = h[8];
    w->rate = (be16(h + 9) << 8) | h[11];
    w->bits = 16;
    w->codec = CODEC_QOA;
    w->data = WAVE_UNKNOWN;
    /* 0 samples = a "streaming" QOA (channels/rate may change): not supported */
    if (w->frames == 0u || w->channels < 1u || w->channels > 2u || w->rate < 1000u || w->rate > 384000u) {
        bad_file();
    }
    src_seek(s, 8);
}

/* ---- decoding: the next frames as 16 bit, channels interleaved --------- */

static const uint16_t ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
};
static const int8_t ima_index[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

typedef struct {
    uint32_t left;          /* frames still to give out (WAVE_UNKNOWN: until EOF) */
    uint32_t data;          /* bytes of the data chunk not read yet */
    /* ADPCM: the block in snd.raw, loaded piece by piece */
    uint32_t blen;          /* bytes in the block */
    uint32_t have;          /* bytes of it read so far */
    uint32_t j;             /* next sample to decode */
    int32_t pred[2];
    int32_t idx[2];
    /* QOA: the frame's slices in snd.raw, a few groups (one slice per
     * channel) at a time */
    uint32_t qsamples;      /* samples per channel left in this frame; 0: header next */
    uint32_t qgroups;       /* slice groups of this frame not read yet */
    uint32_t qpos;          /* the current group's offset in snd.raw */
    uint32_t qsub;          /* next sample in the current slice, 0..19 */
    int32_t lms[2][8];      /* per channel: history[4], weights[4] */
} wave_dec_t;

/* SPEED. Every store to chip RAM is a write-through to the SDRAM, and the C
 * stack lives there too: a store costs far more than an instruction. The
 * loops below therefore keep their state in registers and store each sample
 * once -- normally straight into the PCM engine's buffer (measured on the
 * hardware: ~6.6 us per ADPCM sample with three extra stores per sample). */

/* k samples of one ADPCM channel, from sample j of the block on. grp: the
 * channel's first data byte (block + header + 4 * channel); hd: header size =
 * distance between the channel's 4-byte groups. The difference is
 * (2 * delta + 1) * step / 8, as ffmpeg computes it (encoder and decoder): an
 * ffmpeg file decodes exactly. The IMA reference adds step, step/2, step/4,
 * step/8 truncated one by one; files from such an encoder (sox, Windows) come
 * out a few LSB off. */
__attribute__((noinline)) static void ima_run(const uint8_t *grp, uint32_t hd, uint32_t j, uint32_t k,
    int32_t *pred_io, int32_t *idx_io, int16_t *out) {
    int32_t pred = *pred_io, idx = *idx_io;
    if (k && j == 0u) {
        *out++ = (int16_t)pred;                 /* the header sample */
        j++;
        k--;
    }
    for (uint32_t s = j - 1u, e = s + k; s < e; s++) {
        uint32_t v = grp[(s >> 3) * hd + ((s & 7u) >> 1)];
        uint32_t nib = (s & 1u) ? v >> 4 : v & 15u;
        int32_t diff = (int32_t)((2u * (nib & 7u) + 1u) * ima_step[idx]) >> 3;
        pred += (nib & 8u) ? -diff : diff;
        pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
        idx += ima_index[nib & 7u];
        idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
        *out++ = (int16_t)pred;
    }
    *pred_io = pred;
    *idx_io = idx;
}

/* ---- QOA ---------------------------------------------------------------- */

/* QOA, the "Quite OK Audio" format: the decoder below follows qoa.h by
 * Dominic Szablewski (https://github.com/phoboslab/qoa), whose dequantisation
 * table and LMS predictor it uses:
 *
 *   Copyright (c) 2023 Dominic Szablewski
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to permit
 *   persons to whom the Software is furnished to do so, subject to the
 *   following conditions:
 *
 *   The above copyright notice and this permission notice shall be included
 *   in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 *   NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 *   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 *   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 *   USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Layout (all big endian): file header "qoaf" + samples per channel; frames
 * of an 8-byte header (channels, rate, samples, frame size), 16 bytes of LMS
 * state per channel (history[4], weights[4]) and up to 256 slices per
 * channel, interleaved L R L R. A slice is 64 bits: a 4-bit scale factor and
 * 20 3-bit residuals, first sample in the top bits.
 *
 * Unlike qoa.h, which decodes whole frames into a buffer, this decodes slice
 * by slice from what wave_read() has read: a stereo frame (4 KB) does not
 * have to fit in snd.raw, and the streaming poll reads 2 KB at a time. */

static const int16_t qoa_dequant[16][8] = {
    {   1,    -1,    3,    -3,    5,    -5,     7,     -7},
    {   5,    -5,   18,   -18,   32,   -32,    49,    -49},
    {  16,   -16,   53,   -53,   95,   -95,   147,   -147},
    {  34,   -34,  113,  -113,  203,  -203,   315,   -315},
    {  63,   -63,  210,  -210,  378,  -378,   588,   -588},
    { 104,  -104,  345,  -345,  621,  -621,   966,   -966},
    { 158,  -158,  528,  -528,  950,  -950,  1477,  -1477},
    { 228,  -228,  760,  -760, 1368, -1368,  2128,  -2128},
    { 316,  -316, 1053, -1053, 1895, -1895,  2947,  -2947},
    { 422,  -422, 1405, -1405, 2529, -2529,  3934,  -3934},
    { 548,  -548, 1828, -1828, 3290, -3290,  5117,  -5117},
    { 696,  -696, 2320, -2320, 4176, -4176,  6496,  -6496},
    { 868,  -868, 2893, -2893, 5207, -5207,  8099,  -8099},
    {1064, -1064, 3548, -3548, 6386, -6386,  9933,  -9933},
    {1286, -1286, 4288, -4288, 7718, -7718, 12005, -12005},
    {1536, -1536, 5120, -5120, 9216, -9216, 14336, -14336},
};

/* k samples of one slice (8 bytes at sl), from sample `sub` of it on. lms:
 * the channel's history[4] and weights[4], kept in registers meanwhile. The
 * 64-bit slice is two 32-bit words shifted by constants: no 64-bit shift by a
 * variable, which rv32 would do in libgcc. */
__attribute__((noinline)) static void qoa_run(const uint8_t *sl, uint32_t sub, uint32_t k, int32_t *lms, int16_t *out) {
    uint32_t hi = be32(sl), lo = be32(sl + 4);
    const int16_t *dq = qoa_dequant[hi >> 28];
    hi = (hi << 4) | (lo >> 28);                /* drop the scale factor */
    lo <<= 4;
    for (; sub; sub--) {
        hi = (hi << 3) | (lo >> 29);
        lo <<= 3;
    }
    int32_t h0 = lms[0], h1 = lms[1], h2 = lms[2], h3 = lms[3];
    int32_t w0 = lms[4], w1 = lms[5], w2 = lms[6], w3 = lms[7];
    for (; k; k--) {
        int32_t pred = (w0 * h0 + w1 * h1 + w2 * h2 + w3 * h3) >> 13;
        int32_t r = dq[hi >> 29];
        hi = (hi << 3) | (lo >> 29);
        lo <<= 3;
        int32_t v = pred + r;
        if ((uint32_t)(v + 32768) > 65535u) {
            v = v < 0 ? -32768 : 32767;
        }
        int32_t delta = r >> 4;                 /* sign-sign LMS update */
        w0 += h0 < 0 ? -delta : delta;
        w1 += h1 < 0 ? -delta : delta;
        w2 += h2 < 0 ? -delta : delta;
        w3 += h3 < 0 ? -delta : delta;
        h0 = h1;
        h1 = h2;
        h2 = h3;
        h3 = v;
        *out++ = (int16_t)v;
    }
    lms[0] = h0;
    lms[1] = h1;
    lms[2] = h2;
    lms[3] = h3;
    lms[4] = w0;
    lms[5] = w1;
    lms[6] = w2;
    lms[7] = w3;
}

/* wave_read() for QOA: at most one read per call, as for the others. */
static uint32_t qoa_read(snd_src_t *s, const wav_fmt_t *w, wave_dec_t *d, int16_t *const out[2],
    uint32_t max, uint32_t rbytes) {
    uint32_t ch = w->channels, gs = 8u * ch, n = 0;
    int read = 0;
    while (n < max) {
        if (d->qsamples == 0u) {
            /* the frame header and the LMS state */
            if (read) {
                break;
            }
            uint32_t hl = 8u + 16u * ch;
            uint32_t got = (uint32_t)src_read(s, snd.raw, hl);
            read = 1;
            if (got < hl) {
                d->left = 0;                    /* EOF */
                break;
            }
            const uint8_t *h = snd.raw;
            uint32_t fs = be16(h + 4), groups = (fs + 19u) / 20u;
            if (h[0] != ch || ((be16(h + 1) << 8) | h[3]) != w->rate || fs == 0u || groups > 256u
                || be16(h + 6) != hl + groups * gs) {
                bad_file();
            }
            for (uint32_t c = 0; c < ch; c++) {
                for (uint32_t i = 0; i < 8u; i++) {
                    d->lms[c][i] = (int16_t)be16(h + 8u + 16u * c + 2u * i);
                }
            }
            d->qsamples = fs;
            d->qgroups = groups;
            d->have = d->qpos = d->qsub = 0;
            continue;
        }
        if (d->qpos >= d->have) {
            /* the next slices of this frame */
            if (read) {
                break;
            }
            uint32_t g = rbytes / gs;
            if (g > d->qgroups) {
                g = d->qgroups;
            }
            if (g == 0u) {
                bad_file();                     /* samples left but no slices */
            }
            uint32_t got = (uint32_t)src_read(s, snd.raw, g * gs);
            read = 1;
            got -= got % gs;
            if (got == 0u) {
                d->left = 0;                    /* EOF */
                break;
            }
            d->have = got;
            d->qpos = 0;
            d->qgroups -= got / gs;
            continue;
        }
        uint32_t k = 20u - d->qsub;
        if (k > d->qsamples) {
            k = d->qsamples;
        }
        if (k > max - n) {
            k = max - n;
        }
        for (uint32_t c = 0; c < ch; c++) {
            qoa_run(snd.raw + d->qpos + 8u * c, d->qsub, k, d->lms[c], out[c] + n);
        }
        d->qsub += k;
        d->qsamples -= k;
        n += k;
        if (d->qsub == 20u || d->qsamples == 0u) {
            d->qpos += gs;
            d->qsub = 0;
        }
    }
    if (d->left != WAVE_UNKNOWN) {
        d->left -= n;
    }
    return n;
}

/* Up to max frames as 16 bit, one array per channel (out[1]: stereo only),
 * with at most ONE read of at most rbytes from the source -- the streaming
 * poll calls this and must not block for long. Returns the frames given, 0 if
 * this call only read; d->left == 0 at the end of the data. */
static uint32_t wave_read(snd_src_t *s, const wav_fmt_t *w, wave_dec_t *d, int16_t *const out[2],
    uint32_t max, uint32_t rbytes) {
    uint32_t ch = w->channels;
    if (rbytes > sizeof(snd.raw)) {
        rbytes = sizeof(snd.raw);
    }
    if (max > d->left) {
        max = d->left;
    }
    if (w->codec == CODEC_QOA) {
        return qoa_read(s, w, d, out, max, rbytes);
    }
    if (w->codec == CODEC_PCM) {
        uint32_t bps = w->bits / 8u, frame = ch * bps;
        if (max > rbytes / frame) {
            max = rbytes / frame;
        }
        if (max == 0u) {
            return 0;
        }
        uint32_t got = (uint32_t)src_read(s, snd.raw, max * frame) / frame;
        for (uint32_t c = 0; c < ch; c++) {
            int16_t *o = out[c];
            if (bps == 1u) {
                const uint8_t *p = snd.raw + c;
                for (uint32_t i = 0; i < got; i++, p += frame) {
                    o[i] = (int16_t)(((int32_t)p[0] - 128) << 8);
                }
            } else {
                const uint8_t *p = snd.raw + c * bps + bps - 2u;   /* top 16 bits */
                for (uint32_t i = 0; i < got; i++, p += frame) {
                    o[i] = (int16_t)(p[0] | (p[1] << 8));
                }
            }
        }
        if (d->left != WAVE_UNKNOWN) {
            d->left -= got;
        }
        if (got < max) {
            d->left = 0;                        /* EOF */
        }
        return got;
    }
    /* ADPCM: samples in a block of b bytes = 1 + 8 per whole group of hd */
    uint32_t hd = 4u * ch, n = 0;
    int read = 0;
    while (n < max) {
        uint32_t full = d->blen >= hd ? (d->blen - hd) / hd * 8u + 1u : 0u;
        if (d->have == d->blen && d->j >= full) {
            d->blen = w->align < d->data ? w->align : d->data;    /* the next block */
            d->have = 0;
            d->j = 0;
            if (d->blen < hd) {
                d->left = 0;                    /* end of the data chunk */
                break;
            }
            continue;
        }
        uint32_t avail = d->have >= hd ? (d->have - hd) / hd * 8u + 1u : 0u;
        if (d->j >= avail) {
            if (read) {
                break;                          /* one read per call */
            }
            uint32_t k = d->blen - d->have;
            if (k > rbytes) {
                k = rbytes;
            }
            uint32_t was = d->have;
            uint32_t got = (uint32_t)src_read(s, snd.raw + was, k);
            read = 1;
            if (d->data != WAVE_UNKNOWN) {
                d->data -= got;
            }
            d->have += got;
            if (was < hd && d->have >= hd) {
                for (uint32_t c = 0; c < ch; c++) {
                    d->pred[c] = (int16_t)le16(snd.raw + 4u * c);
                    d->idx[c] = snd.raw[4u * c + 2u] > 88u ? 88 : snd.raw[4u * c + 2u];
                }
            }
            if (got < k) {
                d->blen = d->have;              /* EOF: the block ends here */
                d->data = 0;
                if (d->have < hd) {
                    d->left = 0;
                    break;
                }
            }
            continue;
        }
        uint32_t k = avail - d->j;
        if (k > max - n) {
            k = max - n;
        }
        for (uint32_t c = 0; c < ch; c++) {
            ima_run(snd.raw + hd + 4u * c, hd, d->j, k, &d->pred[c], &d->idx[c], out[c] + n);
        }
        d->j += k;
        n += k;
    }
    if (d->left != WAVE_UNKNOWN) {
        d->left -= n;
    }
    return n;
}

/* Start the engine on the window: channel c at WAV_OFF + c * cap, len
 * samples, played at rate. */
static void pcm_start(uint32_t channels, uint32_t cap, uint32_t len, uint32_t rate, int is16, int loop) {
    /* phase increment = rate / mix rate, Q16 = rate * 65536 * 2083 / 1e8
     * (89465 = that factor in Q16; no 64-bit division -> no libgcc in BRAM) */
    uint32_t pitch = (uint32_t)(((uint64_t)rate * 89465u) >> 16);
    for (uint32_t c = 0; c < 2u; c++) {
        vih_pcm_ch_t *ch = &snd.pcm.ch[c];
        ch->base = (c && channels == 2u) ? WAV_OFF + cap : WAV_OFF;
        ch->len = len;
        ch->pitch = pitch;
        ch->flags.raw = 0;
        ch->flags.f.vol = 255u;
        ch->flags.f.en = c < channels;
        ch->flags.f.is16 = (uint32_t)is16;
        ch->flags.f.loop = loop ? 1u : 0u;
    }
    snd.pcm.header.raw = 0;
    snd.pcm.header.f.master_vol = 255u;
    snd.pcm.header.f.stereo = channels == 2u;
    snd.w_paused = 0;
    snd.w_trig ^= 3u;
    snd.pcm.header.f.trig = snd.w_trig;
    VIH_AUDIO_ENABLE = 1u;
    pcm_commit();
}

static void st_start(snd_src_t *s, const wav_fmt_t *w, mp_obj_t keep, int loop, uint32_t cap, uint32_t ring, uint32_t rbytes);

/* mode: 1 = stream, 0 = never, -1 = when the file does not fit the window.
 * keep: the object to keep alive while streaming (the file or the buffer). */
static void wave_load(snd_src_t *s, int qoa, mp_int_t loop, mp_int_t mode, mp_obj_t keep, uint32_t ring, uint32_t rbytes) {
    wav_fmt_t w;
    if (qoa) {
        qoa_parse(s, &w);
    } else {
        wave_parse(s, &w);
    }

    uint32_t decim = (w.rate + PCM_MAX_RATE - 1u) / PCM_MAX_RATE;
    uint32_t rate = w.rate / decim;
    int known = w.frames != WAVE_UNKNOWN;
    uint32_t want = w.frames / decim;           /* output samples per channel */
    uint32_t cap = (WAV_MAX / w.channels) & ~3u;
    if (mode > 0 || (mode < 0 && (!known || want > cap))) {
        /* longer than even 8 bit fits, or of unknown length: stream it */
        if (decim != 1u) {
            mp_raise_ValueError(MP_ERROR_TEXT("WAV too long (streaming: at most 48 kHz)"));
        }
        st_start(s, &w, keep, (int)loop, cap, ring, rbytes);
        return;
    }
    int out16 = w.bits > 8u || w.codec != CODEC_PCM;
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
    wave_dec_t d = { .left = w.frames, .data = w.data };
    if (out16 && decim == 1u) {
        /* the usual case: decode straight into the engine's buffer */
        while (n < want && d.left) {
            int16_t *const o[2] = { (int16_t *)dst[0] + n, (int16_t *)dst[1] + n };
            n += wave_read(s, &w, &d, o, want - n, sizeof(snd.raw));
        }
    } else {
        /* decimate and/or store 8 bit, through snd.dec */
        uint32_t half = MP_ARRAY_SIZE(snd.dec) / 2u;
        int16_t *const o[2] = { snd.dec, snd.dec + half };
        uint32_t chunk = half / decim * decim;
        uint8_t pend[2] = { 0, 0 };
        while (n < want && d.left) {
            uint32_t got = 0;
            while (got < chunk && d.left) {     /* a whole chunk: decim frames each */
                int16_t *const oo[2] = { o[0] + got, o[1] + got };
                got += wave_read(s, &w, &d, oo, chunk - got, sizeof(snd.raw));
            }
            uint32_t m = n;
            for (uint32_t c = 0; c < w.channels; c++) {
                const int16_t *p = o[c];
                m = n;
                for (uint32_t i = 0; i + decim <= got && m < want; i += decim, m++) {
                    int32_t v = 0;
                    for (uint32_t k = 0; k < decim; k++) {
                        v += *p++;
                    }
                    v /= (int32_t)decim;
                    if (out16) {
                        ((int16_t *)dst[c])[m] = (int16_t)v;
                    } else if (m & 1u) {
                        /* two 8-bit samples per halfword, first sample low */
                        ((uint16_t *)dst[c])[m >> 1] = (uint16_t)(pend[c] | ((uint8_t)(v >> 8) << 8));
                    } else {
                        pend[c] = (uint8_t)(v >> 8);
                    }
                }
            }
            n = m;
            if (got < decim) {
                break;                          /* EOF */
            }
        }
    }
    if (!out16) {
        n &= ~1u;                               /* the engine reads whole words */
    }
    if (n < 2u) {
        bad_file();
    }

    pcm_start(w.channels, cap, n, rate, out16, (int)loop);
    snd.w_on = 1;
    snd.w_loop = loop ? 1 : 0;
    snd.w_start_ms = (uint32_t)mp_hal_ticks_ms();
    snd.w_dur_ms = n / rate * 1000u + (n % rate) * 1000u / rate + 1u;
}

/* ---- streaming: the window as a ring ----------------------------------- */

/* A WAV too long for the window plays through a ring: the window holds
 * `ring` 16-bit samples per channel, the engine loops over it, and
 * vixen_sound_poll() (from uart_poll()) writes the file's next frames
 * behind the engine's position.
 *
 * WHERE IS THE ENGINE? It has no position register. It plays one frame per
 * pitch/65536 mix ticks (100 MHz / 2083), so the position is counted from
 * the raster line clock: 6400/2083 mix ticks per 64-us line. When C code
 * blocks the polling, ticks_raster_lines() lags and the count with it -- the
 * safe direction: we believe the engine is behind where it is, so we refill
 * late, never over samples it has yet to play. The count may run ahead only
 * by a clock mismatch; `guard` (1/8 of the ring) keeps a distance to that.
 *
 * HOW FAST. Each poll reads at most ST_READ bytes (one kernel call) and
 * decodes what they hold. Measured on the hardware: a separate read costs
 * ~5 ms on top of the transfer, so 1 KB per read takes ~26 % of the CPU for
 * 22 kHz stereo ADPCM, 512 bytes ~36 %. 48 kHz stereo QOA with a SID tune
 * playing, Python's share in the steady state / the longest block of one
 * poll: 1 KB 51 % / 11 ms, 2 KB 60 % / 14 ms, 4 KB 70 % / 20 ms. A block
 * longer than the SID player's 4 ms lead delays its writes; 2 KB is the
 * compromise. While less than the lead is buffered, every poll refills;
 * above it, one poll per frame (20 ms), so the full ring builds up slowly.
 * The lead itself grows: ST_HEAD at the start, then half a second per second
 * played, up to ST_LEAD. Filling the whole ST_LEAD at once took the first
 * ~4 s of the CPU at 48 kHz stereo (sound_demo stuttered meanwhile); now the
 * start costs little more than the steady state.
 * At the end of the file the ring is filled with silence and the engine is
 * stopped when the count reaches the last frame. */

#define ST_READ     2048u       /* default; play(_chunk=) for measurements */
#define ST_LEAD     8u          /* seconds */
#define ST_HEAD     1u          /* seconds decoded before play() returns */

static struct {
    uint8_t on;
    uint8_t eof;
    uint8_t loop;
    uint8_t busy;
    snd_src_t src;
    wav_fmt_t w;
    wave_dec_t d;
    size_t data_pos;            /* the data chunk's offset in the file */
    int16_t *base[2];
    uint32_t ring, guard, lead; /* frames */
    uint32_t head;              /* frames buffered at the start */
    uint32_t wr;                /* frames written since the start */
    uint32_t pass;              /* frames written since the last loop restart */
    uint32_t end;               /* eof: the frame the sound ends with */
    uint32_t played;            /* frames the engine has played (estimate) */
    uint32_t frac;              /* ... its fraction, Q16 */
    uint32_t mix_rem;           /* mix ticks * 2083 not counted yet */
    uint32_t lines;             /* raster line count at the last update */
    uint32_t frame;             /* raster frame of the last background refill */
    uint32_t pitch;
    uint32_t rbytes;            /* bytes per read in the poll */
} st;

MP_REGISTER_ROOT_POINTER(mp_obj_t vixen_sound_stream);

static void st_clock(void) {
    uint32_t now = (uint32_t)ticks_raster_lines();
    uint32_t dl = now - st.lines;
    st.lines = now;
    while (dl) {
        uint32_t k = dl < 0x10000u ? dl : 0x10000u;     /* k * 6400 < 2^32 */
        dl -= k;
        uint32_t m = st.mix_rem + k * 6400u;
        uint32_t mix = m / PCM_DIV;
        st.mix_rem = m % PCM_DIV;
        uint64_t x = (uint64_t)mix * st.pitch + st.frac;
        st.played += (uint32_t)(x >> 16);
        st.frac = (uint32_t)x & 0xFFFFu;
    }
}

/* Let go of the source. close: also close the file (not at soft reset: the
 * gc finalisers closed it already). */
static void st_release(int close) {
    vixen_sound_active &= (uint8_t)~2u;
    st.on = 0;
    if (close) {
        src_close(&st.src);
    }
    st.src.file = MP_OBJ_NULL;
    st.src.buf = NULL;
    MP_STATE_VM(vixen_sound_stream) = MP_OBJ_NULL;
}

/* Write the next frames behind the engine: at most one read of rbytes. */
static void st_fill(uint32_t rbytes, uint32_t lead) {
    int32_t room = (int32_t)(st.played + lead - st.wr);
    if (room <= 0) {
        return;
    }
    uint32_t at = st.wr % st.ring;
    uint32_t k = st.ring - at;                  /* up to the end of the ring */
    if (k > (uint32_t)room) {
        k = (uint32_t)room;
    }
    int16_t *const o[2] = { st.base[0] + at, st.base[1] + at };
    if (st.eof) {
        if (k > 1024u) {
            k = 1024u;
        }
        for (uint32_t c = 0; c < st.w.channels; c++) {
            memset(o[c], 0, k * 2u);            /* silence after the end */
        }
        st.wr += k;
        return;
    }
    uint32_t got = wave_read(&st.src, &st.w, &st.d, o, k, rbytes);
    st.wr += got;
    st.pass += got;
    if (st.d.left == 0u) {
        if (st.loop && st.pass) {
            src_seek(&st.src, st.data_pos);     /* again from the start */
            st.d = (wave_dec_t) { .left = st.w.frames, .data = st.w.data };
            st.pass = 0;
        } else {
            st.eof = 1;
            st.end = st.wr;
            src_close(&st.src);                 /* the card is free again */
            MP_STATE_VM(vixen_sound_stream) = MP_OBJ_NULL;
        }
    }
}

static void st_stop(void) {
    st_release(1);
    wave_stop_internal();
}

static void st_service(void) {
    st_clock();
    if (st.eof && (int32_t)(st.played - st.end) >= 0) {
        st_stop();
        return;
    }
    uint32_t lead = st.head + st.played / 2u;  /* grows to st.lead */
    if (lead > st.lead) {
        lead = st.lead;
    }
    if ((int32_t)(st.wr - st.played) < (int32_t)lead) {
        st_fill(st.rbytes, lead);               /* below the lead: every poll */
    } else {
        uint32_t frame = st.lines / 312u;
        if (frame != st.frame) {
            st.frame = frame;                   /* above: once per frame */
            st_fill(st.rbytes, st.ring - st.guard);
        }
    }
}

/* From vixen_sound_poll(). A read error ends the stream (and the sound). */
static void st_poll(void) {
    if (st.busy) {
        return;
    }
    st.busy = 1;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        st_service();
        nlr_pop();
    } else {
        st_release(0);
        st.src.file = MP_OBJ_NULL;
        wave_stop_internal();
    }
    st.busy = 0;
}

/* ring: frames per channel, 0 = the whole window (a smaller ring is for the
 * tests). Takes over s (its file stays open while the sound plays). */
static void st_start(snd_src_t *s, const wav_fmt_t *w, mp_obj_t keep, int loop, uint32_t cap, uint32_t ring, uint32_t rbytes) {
    st.rbytes = (rbytes >= 64u && rbytes <= sizeof(snd.raw)) ? rbytes : ST_READ;
    st.src = *s;
    s->file = MP_OBJ_NULL;                      /* ours now: play() must not close it */
    MP_STATE_VM(vixen_sound_stream) = keep;
    st.w = *w;
    st.d = (wave_dec_t) { .left = w->frames, .data = w->data };
    st.data_pos = st.src.pos;
    st.ring = (ring && ring < cap / 2u) ? ring : cap / 2u;
    st.guard = st.ring / 8u;
    st.lead = w->rate * ST_LEAD;
    if (st.lead > st.ring - st.guard) {
        st.lead = st.ring - st.guard;
    }
    st.base[0] = (int16_t *)CHIP(WAV_OFF);
    st.base[1] = (int16_t *)CHIP(WAV_OFF + cap);
    st.loop = (uint8_t)loop;
    st.eof = 0;
    st.wr = st.pass = st.end = 0;
    st.played = st.frac = st.mix_rem = 0;

    /* a head start, then the engine */
    uint32_t head = w->rate * ST_HEAD;
    if (head > st.lead) {
        head = st.lead;
    }
    st.head = head;
    while (st.wr < head && !st.eof) {
        st_fill(sizeof(snd.raw), head);
    }
    if (st.eof && st.end < 2u) {
        st_release(1);
        bad_file();
    }
    st.pitch = (uint32_t)(((uint64_t)w->rate * 89465u) >> 16);
    pcm_start(w->channels, cap, st.ring, w->rate, 1, 1);
    st.lines = (uint32_t)ticks_raster_lines();
    st.frame = st.lines / 312u;
    st.on = 1;
    snd.w_on = 1;
    snd.w_loop = (uint8_t)loop;
    vixen_sound_active |= 2u;
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

/* The source while play() loads. NOT a local: nlr_push() returns twice, gcc
 * does not know it, and kept a copy of a local's file pointer in a stack slot
 * that the inlined loader then reused -- the exception path closed garbage.
 * A static is read back from memory after nlr_push(). The object itself is
 * held by a root pointer, so a collection during the load cannot free it. */
static snd_src_t ld;
MP_REGISTER_ROOT_POINTER(mp_obj_t vixen_sound_load);

static void ld_open(mp_obj_t src) {
    src_open(&ld, src);
    MP_STATE_VM(vixen_sound_load) = ld.file != MP_OBJ_NULL ? ld.file : src;
}

static void ld_close(void) {
    MP_STATE_VM(vixen_sound_load) = MP_OBJ_NULL;
    src_close(&ld);
}

/* wav.play() and qoa.play(): one PCM engine, so either replaces the other. */
static mp_obj_t snd_pcm_play(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args, int qoa) {
    enum { ARG_src, ARG_loop, ARG_stream, ARG__ring, ARG__chunk };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_loop, MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_stream, MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR__ring, MP_ARG_INT, {.u_int = 0} },   /* tests: ring size in frames */
        { MP_QSTR__chunk, MP_ARG_INT, {.u_int = 0} },  /* measurements: bytes per poll read */
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    mp_int_t mode = args[ARG_stream].u_obj == mp_const_none ? -1 : mp_obj_is_true(args[ARG_stream].u_obj);

    if (st.on) {
        st_release(1);
    }
    wave_stop_internal();                       /* the engine must not read what we overwrite */
    ld_open(args[ARG_src].u_obj);
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        wave_load(&ld, qoa, args[ARG_loop].u_bool, mode, MP_STATE_VM(vixen_sound_load),
            (uint32_t)args[ARG__ring].u_int, (uint32_t)args[ARG__chunk].u_int);
        nlr_pop();
    } else {
        ld_close();
        nlr_jump(nlr.ret_val);
    }
    ld_close();                                 /* streaming: st_start() took the file */
    return mp_const_none;
}

static mp_obj_t snd_wave_play(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    return snd_pcm_play(n_args, pos_args, kw_args, 0);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(snd_wave_play_obj, 1, snd_wave_play);

static mp_obj_t snd_qoa_play(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    return snd_pcm_play(n_args, pos_args, kw_args, 1);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(snd_qoa_play_obj, 1, snd_qoa_play);

static mp_obj_t snd_wave_stop(void) {
    if (st.on) {
        st_release(1);
    }
    wave_stop_internal();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(snd_wave_stop_obj, snd_wave_stop);

static mp_obj_t snd_wave_playing(void) {
    if (st.on) {
        st_clock();
        if (st.eof && (int32_t)(st.played - st.end) >= 0) {
            st_stop();
        }
        return mp_obj_new_bool(st.on);
    }
    if (snd.w_on && !snd.w_loop && !snd.w_paused && (uint32_t)mp_hal_ticks_ms() - snd.w_start_ms >= snd.w_dur_ms) {
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
    ld_open(args[ARG_src].u_obj);
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        sid_load(&ld, args[ARG_song].u_int);
        nlr_pop();
    } else {
        ld_close();
        nlr_jump(nlr.ret_val);
    }
    ld_close();
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
    return mp_obj_new_bool((vixen_sound_active & 1u) || snd.s_paused);
}
static MP_DEFINE_CONST_FUN_OBJ_0(snd_sid_playing_obj, snd_sid_playing);

/* ---- pause / resume ------------------------------------------------------
 * WAV/QOA: the engine reads its control words live and keeps its position
 * while the pitch is 0 (no retrigger: the trigger nibble stays), so a pause
 * is pitch 0 and volume 0. The streaming position count stops with it
 * (st.pitch 0); a loaded sound's end time moves by the pause.
 * SID: the player is not polled, the SID's master volume goes to 0; on
 * resume the player's clock and its queued writes move by the pause. The
 * voices' envelopes run on meanwhile (a held note is still held). */
static mp_obj_t snd_wave_pause(void) {
    if (snd.w_on && !snd.w_paused) {
        if (st.on) {
            st_clock();                         /* count up to now, then stop */
            st.pitch = 0;
        }
        snd.w_pitch = snd.pcm.ch[0].pitch;
        for (uint32_t c = 0; c < 2u; c++) {
            snd.pcm.ch[c].pitch = 0;
            snd.pcm.ch[c].flags.f.vol = 0;
        }
        pcm_commit();
        snd.w_pause_ms = (uint32_t)mp_hal_ticks_ms();
        snd.w_paused = 1;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(snd_wave_pause_obj, snd_wave_pause);

static mp_obj_t snd_wave_resume(void) {
    if (snd.w_paused) {
        snd.w_paused = 0;
        for (uint32_t c = 0; c < 2u; c++) {
            snd.pcm.ch[c].pitch = snd.w_pitch;
            snd.pcm.ch[c].flags.f.vol = 255u;
        }
        if (st.on) {
            st_clock();                         /* the paused lines count 0 */
            st.pitch = snd.w_pitch;
        } else {
            snd.w_start_ms += (uint32_t)mp_hal_ticks_ms() - snd.w_pause_ms;
        }
        pcm_commit();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(snd_wave_resume_obj, snd_wave_resume);

static mp_obj_t snd_wave_paused(void) {
    return mp_obj_new_bool(snd.w_paused);
}
static MP_DEFINE_CONST_FUN_OBJ_0(snd_wave_paused_obj, snd_wave_paused);

static mp_obj_t snd_sid_pause(void) {
    if ((vixen_sound_active & 1u) && !snd.s_paused) {
        vixen_sound_active &= (uint8_t)~1u;
        snd.s_paused = 1;
        snd.s_pause_t = snd_now();
        sidp_out(24u, sidp.io[0x418u] & 0xF0u);    /* volume 0, filter mode kept */
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(snd_sid_pause_obj, snd_sid_pause);

static mp_obj_t snd_sid_resume(void) {
    if (snd.s_paused) {
        uint32_t d = snd_now() - snd.s_pause_t;
        sidp.due += d;
        sidp.frame0 += d;
        sidp.cia_last += d;
        for (uint32_t i = sidp.qhead; i != sidp.qtail; i++) {
            sidp.q[i & (SIDP_QLEN - 1u)].t += d;
        }
        sidp_out(24u, sidp.io[0x418u]);
        snd.s_paused = 0;
        vixen_sound_active |= 1u;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(snd_sid_resume_obj, snd_sid_resume);

static mp_obj_t snd_sid_paused(void) {
    return mp_obj_new_bool(snd.s_paused);
}
static MP_DEFINE_CONST_FUN_OBJ_0(snd_sid_paused_obj, snd_sid_paused);

/* Soft reset (main.c): silence what we used, touch nothing else. The
 * chip-RAM window holds no Python objects. */
void vixen_sound_reset(void) {
    sid_stop_internal();
    st_release(0);                              /* the finalisers closed the file */
    if (snd.pcm.magic == VIH_PCM_MAGIC) {
        wave_stop_internal();                   /* only if we ever took the PCM engine */
    }
}

static const mp_rom_map_elem_t snd_wave_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_wav) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&snd_wave_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&snd_wave_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_playing), MP_ROM_PTR(&snd_wave_playing_obj) },
    { MP_ROM_QSTR(MP_QSTR_pause), MP_ROM_PTR(&snd_wave_pause_obj) },
    { MP_ROM_QSTR(MP_QSTR_resume), MP_ROM_PTR(&snd_wave_resume_obj) },
    { MP_ROM_QSTR(MP_QSTR_paused), MP_ROM_PTR(&snd_wave_paused_obj) },
};
static MP_DEFINE_CONST_DICT(snd_wave_globals, snd_wave_globals_table);
static const mp_obj_module_t snd_wave_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&snd_wave_globals,
};

/* sound.qoa: QOA files on the same engine; everything but play() is wav's */
static const mp_rom_map_elem_t snd_qoa_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_qoa) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&snd_qoa_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&snd_wave_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_playing), MP_ROM_PTR(&snd_wave_playing_obj) },
    { MP_ROM_QSTR(MP_QSTR_pause), MP_ROM_PTR(&snd_wave_pause_obj) },
    { MP_ROM_QSTR(MP_QSTR_resume), MP_ROM_PTR(&snd_wave_resume_obj) },
    { MP_ROM_QSTR(MP_QSTR_paused), MP_ROM_PTR(&snd_wave_paused_obj) },
};
static MP_DEFINE_CONST_DICT(snd_qoa_globals, snd_qoa_globals_table);
static const mp_obj_module_t snd_qoa_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&snd_qoa_globals,
};

static const mp_rom_map_elem_t snd_sid_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_sid) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&snd_sid_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&snd_sid_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_playing), MP_ROM_PTR(&snd_sid_playing_obj) },
    { MP_ROM_QSTR(MP_QSTR_pause), MP_ROM_PTR(&snd_sid_pause_obj) },
    { MP_ROM_QSTR(MP_QSTR_resume), MP_ROM_PTR(&snd_sid_resume_obj) },
    { MP_ROM_QSTR(MP_QSTR_paused), MP_ROM_PTR(&snd_sid_paused_obj) },
};
static MP_DEFINE_CONST_DICT(snd_sid_globals, snd_sid_globals_table);
static const mp_obj_module_t snd_sid_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&snd_sid_globals,
};

static const mp_rom_map_elem_t snd_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_sound) },
    { MP_ROM_QSTR(MP_QSTR_wav), MP_ROM_PTR(&snd_wave_module) },
    { MP_ROM_QSTR(MP_QSTR_qoa), MP_ROM_PTR(&snd_qoa_module) },
    { MP_ROM_QSTR(MP_QSTR_sid), MP_ROM_PTR(&snd_sid_module) },
};
static MP_DEFINE_CONST_DICT(snd_globals, snd_globals_table);

const mp_obj_module_t mp_module_sound = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&snd_globals,
};

MP_REGISTER_MODULE(MP_QSTR_sound, mp_module_sound);

#endif

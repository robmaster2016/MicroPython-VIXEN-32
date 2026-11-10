/* SPDX-License-Identifier: MIT
 * Copyright (c) 2025-2026 Robert S. (aka Robmaster) */
/*
 * vx_sidplay.h -- PSID/RSID player on vx6502.h, without MicroPython.
 *
 * Included once by vixen_sound.c and by the PC test harness
 * (tools/sidtest/sidplay.c). The includer defines, before including:
 *
 *   static void sidp_out(unsigned reg, unsigned v)       write a SID register
 *   static void sidp_out_ctrl(unsigned reg, unsigned v)  same, control
 *                                         register (4/11/18): gate/test edges
 *
 * Use: sidp_parse() the header, copy the C64 data to ram[load], then
 * sidp_setup(), sidp_start(now) and sidp_poll(now) as often as possible.
 *
 * TIMING. A call runs in one go, much faster than a C64 would. Every SID
 * write is therefore stamped with its C64 time -- the call's scheduled time
 * plus the 6502 cycles up to the write -- and queued; sidp_poll() hands the
 * queue to the SID when the clock gets there. Calls run SIDP_LEAD ahead of
 * their time, so how late the host polls (VM hook, a slow C call up to that
 * lead) does not move the call, only the poll granularity remains. Raster
 * reads ($D011/$D012) and the CIA 1 timer A counter ($DC04/5) follow the
 * same virtual clock, so a tune that waits for a line or times a digi loop
 * sees real time pass.
 * Time is in units of 0.25 us (1/256 of a 64 us VIXEN raster line), as a
 * wrapping 32-bit count (18 minutes per lap): all comparisons are on
 * differences, and nothing here needs 64-bit division -- libgcc's would be
 * linked into the BRAM.
 *
 * The C64 as the tune sees it: 64 KB RAM, no ROMs (the few KERNAL entry
 * points an interrupt handler leaves through are put in RAM), the SID at
 * $D400, and just enough of the VIC and CIA 1 to drive the play routine:
 *
 *  - play address != 0 (most PSIDs): called like JSR, once per frame or at
 *    the CIA 1 timer A rate, as the speed flags say.
 *  - play address 0 (RSIDs, some PSIDs): init installs an interrupt handler
 *    and programs a source. Raster compare ($D012/$D011 bit 7 with $D01A bit
 *    0) fires when a virtual beam reaches the line, CIA 1 timer A ($DC0D mask)
 *    every latch+1 cycles; whichever comes first is delivered through $0314
 *    (KERNAL banked in) or $FFFE. $D019/$DC0D read back the source. The
 *    KERNAL's state is the start: CIA timer A IRQ on at 60 Hz.
 *
 * Not emulated: the main program between interrupts (an init that never
 * returns is left after SIDP_INIT_BUDGET cycles), NMIs (CIA 2 digis),
 * the second and third SID, the BASIC interpreter (RSID BASIC tunes).
 */

#ifndef VX_SIDPLAY_H
#define VX_SIDPLAY_H

#include <stdint.h>
#include <string.h>

#include "vx6502.h"

#define SIDP_TRAP        0xDFF0u   /* return address of every call (I/O area) */
#define SIDP_INIT_BUDGET 3000000u  /* cycles: init may decrunch, ~3 C64 seconds */
#define SIDP_PLAY_BUDGET 40000u    /* cycles: two PAL frames */
#define SIDP_MAX_FAILS   50u       /* calls in a row that did not return -> stuck */
#define SIDP_UNITS_PER_S 4000000u
#define SIDP_LEAD        16000u    /* run calls 4 ms ahead of their time */
#define SIDP_QLEN        1024u     /* queued SID writes (power of two) */
#define SIDP_MAX_AHEAD   4         /* calls per sidp_poll() at most */

typedef struct {
    uint32_t t;                    /* when it is due */
    uint8_t reg, val;
} sidp_ev_t;

typedef struct {
    uint16_t version, off, load, init, play, songs, start, flags;
    uint32_t speed;
    uint8_t rsid;
    uint8_t sid2, sid3;            /* $Dxx0 >> 4 of extra SIDs, 0 = none */
} sidp_hdr_t;

static struct {
    vx6502_t cpu;
    uint8_t *io;                   /* 4 KB shadow of $D000-$DFFF */
    uint16_t init;
    uint16_t play;                 /* 0 = interrupt driven */
    uint16_t sid2, sid3;           /* extra SID bases, writes ignored */
    uint16_t freq[3];              /* the tune's frequency registers, unscaled */
    uint32_t fscale;               /* tune clock / 1 MHz, Q16 */
    uint32_t upc;                  /* time units per 6502 cycle, Q16 */
    uint32_t clock;                /* 6502 clock, Hz */
    uint32_t line_cycles;          /* PAL 63, NTSC 65 */
    uint32_t lines;                /* PAL 312, NTSC 263 */
    uint8_t cia_speed;             /* play address: CIA timer instead of frame */
    uint8_t fails;
    uint8_t cia_mask;              /* CIA 1 ICR mask (bit 0 = timer A) */
    uint8_t vic_mask;              /* $D01A (bit 0 = raster) */
    uint8_t cia_icr;               /* $DC0D read value (cleared by the read) */
    uint8_t vic_irq;               /* $D019 read value */
    uint8_t next_src;              /* 1 = raster, 2 = CIA, 0 = frame fallback */
    uint8_t song;
    uint8_t direct;                /* writes go out at once (init, silence) */
    uint32_t ncalls[3];            /* calls by source (frame/raster/CIA), for tests */
    uint32_t rnd;
    uint32_t cpu_q16;              /* 6502 cycles per time unit, Q16 */
    uint32_t call_t;               /* the running call's scheduled time */
    uint32_t due;                  /* next call */
    uint32_t frame0;               /* start of a virtual frame */
    uint32_t cia_last;             /* last timer A underflow */
    uint32_t qhead, qtail;         /* free-running indices into q */
    sidp_ev_t q[SIDP_QLEN];
} sidp;

static uint32_t sidp_units(uint32_t cycles) {
    return (uint32_t)(((uint64_t)cycles * sidp.upc) >> 16);
}

static uint32_t sidp_cycles(uint32_t units) {
    return (uint32_t)(((uint64_t)units * sidp.cpu_q16) >> 16);
}

#define SIDP_AFTER(a, b) ((int32_t)((a) - (b)) > 0)   /* a later than b */

/* The C64 time of the instruction now running in a call. */
static uint32_t sidp_tnow(void) {
    return sidp.call_t + sidp_units(sidp.cpu.cyc);
}

/* The virtual beam: line 0..lines-1 at the current C64 time. */
static uint32_t sidp_beam(void) {
    uint32_t c = sidp_cycles(sidp_tnow() - sidp.frame0);
    return (c / sidp.line_cycles) % sidp.lines;
}

static uint32_t sidp_be16(const uint8_t *p) {
    return ((uint32_t)p[0] << 8) | p[1];
}

/* b holds 0x76 bytes, or 0x7C when the header says version >= 2 and a data
 * offset >= 0x7C (read 0x76, check sidp_hdr_len(), read the rest).
 * Returns 0, or -1 when this is no SID file. */
static unsigned sidp_hdr_len(const uint8_t *b) {
    return (sidp_be16(b + 4) >= 2u && sidp_be16(b + 6) >= 0x7Cu) ? 0x7Cu : 0x76u;
}

static int sidp_parse(sidp_hdr_t *h, const uint8_t *b) {
    if (memcmp(b, "PSID", 4) && memcmp(b, "RSID", 4)) {
        return -1;
    }
    memset(h, 0, sizeof(*h));
    h->rsid = b[0] == 'R';
    h->version = (uint16_t)sidp_be16(b + 4);
    h->off = (uint16_t)sidp_be16(b + 6);
    h->load = (uint16_t)sidp_be16(b + 8);
    h->init = (uint16_t)sidp_be16(b + 10);
    h->play = (uint16_t)sidp_be16(b + 12);
    h->songs = (uint16_t)sidp_be16(b + 14);
    h->start = (uint16_t)sidp_be16(b + 16);
    h->speed = (sidp_be16(b + 18) << 16) | sidp_be16(b + 20);
    if (h->off < 0x76u) {
        return -1;
    }
    if (sidp_hdr_len(b) == 0x7Cu) {
        h->flags = (uint16_t)sidp_be16(b + 0x76);
        h->sid2 = b[0x7A];
        h->sid3 = b[0x7B];
    }
    if (h->songs == 0u) {
        h->songs = 1u;
    }
    return 0;
}

/* ---- SID writes ---------------------------------------------------------- */

static void sidp_put(unsigned reg, unsigned v) {
    unsigned k = reg % 7u;
    if (reg < 21u && k == 4u) {
        sidp_out_ctrl(reg, v);
    } else {
        sidp_out(reg, v);
    }
}

static void sidp_emit(unsigned reg, unsigned v) {
    if (sidp.direct) {
        sidp_put(reg, v);
        return;
    }
    if (sidp.qtail - sidp.qhead >= SIDP_QLEN) { /* full: the oldest goes out now */
        sidp_ev_t *e = &sidp.q[sidp.qhead++ & (SIDP_QLEN - 1u)];
        sidp_put(e->reg, e->val);
    }
    sidp_ev_t *e = &sidp.q[sidp.qtail++ & (SIDP_QLEN - 1u)];
    e->t = sidp_tnow();
    e->reg = (uint8_t)reg;
    e->val = (uint8_t)v;
}

/* Hand every queued write that is due by `now` to the SID. */
static void sidp_flush(uint32_t now) {
    while (sidp.qhead != sidp.qtail) {
        sidp_ev_t *e = &sidp.q[sidp.qhead & (SIDP_QLEN - 1u)];
        if (SIDP_AFTER(e->t, now)) {
            break;
        }
        sidp.qhead++;
        sidp_put(e->reg, e->val);
    }
}

static void sidp_write(unsigned r, uint8_t v) {
    if (r > 24u) {
        return;                                 /* 25..28 are read-only */
    }
    unsigned voice = r / 7u, k = r % 7u;
    if (voice < 3u && k < 2u) {
        /* frequency lo/hi: scaled from the tune's clock to the 1 MHz SID */
        uint16_t f = sidp.freq[voice];
        f = k ? (uint16_t)((f & 0x00FFu) | (v << 8)) : (uint16_t)((f & 0xFF00u) | v);
        sidp.freq[voice] = f;
        uint32_t sc = ((uint32_t)f * sidp.fscale + 0x8000u) >> 16;
        if (sc > 0xFFFFu) {
            sc = 0xFFFFu;
        }
        sidp_emit(voice * 7u, sc & 0xFFu);
        sidp_emit(voice * 7u + 1u, sc >> 8);
    } else {
        sidp_emit(r, v);
    }
}

static void sidp_silence(void) {
    sidp.qhead = sidp.qtail;                    /* drop what is queued */
    for (unsigned r = 0; r <= 24u; r++) {
        sidp_out(r, 0);
    }
}

/* ---- the I/O area -------------------------------------------------------- */

static uint8_t vx_io_rd(uint16_t addr) {
    uint8_t *io = sidp.io;
    uint32_t v;
    switch (addr) {
        case 0xD011:
            return (uint8_t)((io[0x011] & 0x7Fu) | ((sidp_beam() & 0x100u) >> 1));
        case 0xD012:
            return (uint8_t)sidp_beam();
        case 0xD019:
            return (uint8_t)(sidp.vic_irq | 0x70u);
        case 0xDC04:                            /* timer A counts down from the latch */
        case 0xDC05: {
            uint32_t latch = (uint32_t)(io[0xC04] | (io[0xC05] << 8)) + 1u;
            v = latch - 1u - sidp_cycles(sidp_tnow() - sidp.cia_last) % latch;
            return (uint8_t)(addr == 0xDC04u ? v : v >> 8);
        }
        case 0xD41B:                            /* OSC3: tunes use it as noise */
            sidp.rnd = sidp.rnd * 1103515245u + 12345u;
            return (uint8_t)(sidp.rnd >> 16);
        case 0xD41C:                            /* ENV3 */
            return 0;
        case 0xDC0D:                            /* reading acknowledges */
            v = sidp.cia_icr;
            sidp.cia_icr = 0;
            return (uint8_t)v;
        case 0xDD0D:
            return 0;
        default:
            return io[addr & 0x0FFFu];
    }
}

static void vx_io_wr(uint16_t addr, uint8_t v) {
    if (addr >= 0xD400u && addr < 0xD800u) {
        uint16_t b = addr & 0xFFE0u;
        if ((sidp.sid2 && b == sidp.sid2) || (sidp.sid3 && b == sidp.sid3)) {
            return;
        }
        sidp.io[0x400u + (addr & 0x1Fu)] = v;
        sidp_write(addr & 0x1Fu, v);
        return;
    }
    switch (addr) {
        case 0xDC0D:                            /* ICR mask: bit 7 = set/clear */
            if (v & 0x80u) {
                sidp.cia_mask |= v & 0x1Fu;
            } else {
                sidp.cia_mask &= (uint8_t)~v;
            }
            return;
        case 0xD01A:
            sidp.vic_mask = v & 0x0Fu;
            return;
        case 0xD019:                            /* acknowledge */
            sidp.vic_irq &= (uint8_t)~(v & 0x0Fu);
            if (!(sidp.vic_irq & 0x0Fu)) {
                sidp.vic_irq = 0;
            }
            return;
        case 0xDD0D:
            return;
        default:
            sidp.io[addr & 0x0FFFu] = v;        /* incl. the timer A latch $DC04/5 */
            return;
    }
}

/* ---- calling into the tune ----------------------------------------------- */

static int sidp_call(uint16_t addr, uint8_t a, uint32_t budget) {
    vx6502_t *c = &sidp.cpu;
    uint32_t used;
    c->ram[0x1FFu] = (SIDP_TRAP - 1u) >> 8;     /* as if JSR'd from SIDP_TRAP */
    c->ram[0x1FEu] = (SIDP_TRAP - 1u) & 0xFFu;
    c->s = 0xFDu;
    c->a = a;
    c->x = 0;
    c->y = 0;
    c->p = VX_U | VX_I;
    c->pc = addr;
    return vx6502_run(c, SIDP_TRAP, budget, &used);
}

/* An interrupt: with the KERNAL banked in through $0314 (the KERNAL's entry
 * pushes A/X/Y first; handlers leave through $EA31/$EA81), else $FFFE. */
static int sidp_irq(uint32_t budget) {
    vx6502_t *c = &sidp.cpu;
    uint8_t *m = c->ram;
    uint32_t used;
    c->s = 0xFFu;
    m[0x100u + c->s--] = SIDP_TRAP >> 8;
    m[0x100u + c->s--] = SIDP_TRAP & 0xFFu;
    m[0x100u + c->s--] = VX_U;
    if (m[1] & 2u) {
        m[0x100u + c->s--] = c->a;
        m[0x100u + c->s--] = c->x;
        m[0x100u + c->s--] = c->y;
        c->pc = (uint16_t)(m[0x314] | (m[0x315] << 8));
    } else {
        c->pc = (uint16_t)(m[0xFFFE] | (m[0xFFFF] << 8));
    }
    c->p = VX_U | VX_I;
    return vx6502_run(c, SIDP_TRAP, budget, &used);
}

static uint32_t sidp_cia_cycles(void) {
    uint32_t c = (uint32_t)(sidp.io[0xC04] | (sidp.io[0xC05] << 8)) + 1u;
    return c < 256u ? 256u : c;                 /* a tiny latch would starve the host */
}

/* Schedule the call after the one at sidp.due. */
static void sidp_schedule(void) {
    uint32_t t = sidp.due;
    uint32_t frame = sidp_units(sidp.line_cycles * sidp.lines);
    sidp.frame0 = t - (t - sidp.frame0) % frame;   /* the current frame */
    if (sidp.play) {
        sidp.due = t + (sidp.cia_speed ? sidp_units(sidp_cia_cycles()) : frame);
        return;
    }
    uint32_t best = t + frame;
    sidp.next_src = 0;
    if (sidp.vic_mask & 1u) {
        uint32_t cmp = sidp.io[0x012] | ((sidp.io[0x011] & 0x80u) << 1);
        if (cmp < sidp.lines) {
            uint32_t at = sidp.frame0 + sidp_units(cmp * sidp.line_cycles);
            if (!SIDP_AFTER(at, t)) {
                at += frame;
            }
            best = at;
            sidp.next_src = 1;
        }
    }
    if (sidp.cia_mask & 1u) {
        uint32_t per = sidp_units(sidp_cia_cycles());
        uint32_t at = sidp.cia_last + per;
        while (!SIDP_AFTER(at, t)) {
            sidp.cia_last = at;
            at += per;
        }
        if (sidp.next_src == 0 || SIDP_AFTER(best, at)) {
            best = at;
            sidp.next_src = 2;
        }
    } else {
        sidp.cia_last = t;                      /* counts from when it is enabled */
    }
    sidp.due = best;
}

/* After the header was parsed and the data copied to ram[load..load+len):
 * set up the machine for `song` (0 = the file's start song). Returns the
 * song number, or -1 when there is no such song. */
static int sidp_setup(const sidp_hdr_t *h, uint8_t *ram, uint8_t *io, uint32_t load, uint32_t len, int song) {
    if (song <= 0) {
        song = h->start ? h->start : 1;
    }
    if (song > h->songs) {
        return -1;
    }
    uint8_t *m = ram;
    uint32_t end = load + len;
    uint32_t init = h->init ? h->init : load;
    memset(&sidp, 0, sizeof(sidp));
    sidp.cpu.ram = ram;
    sidp.io = io;
    memset(io, 0, 0x1000u);

    /* KERNAL pieces in RAM, where the tune does not sit:
     *   $EA31 JMP $EA81, $EA7E LDA $DC0D, $EA81 PLA TAY PLA TAX PLA RTI,
     *   $FEBC PLA TAY PLA TAX PLA RTI, vector $0314 = $EA31 */
    static const uint8_t irq_exit[] = { 0x68, 0xA8, 0x68, 0xAA, 0x68, 0x40 };
    if (end <= 0xEA31u || load > 0xEA87u) {
        static const uint8_t ea31[] = { 0x4C, 0x81, 0xEA };
        static const uint8_t ea7e[] = { 0xAD, 0x0D, 0xDC };
        memcpy(m + 0xEA31, ea31, 3);
        memcpy(m + 0xEA7E, ea7e, 3);
        memcpy(m + 0xEA81, irq_exit, sizeof(irq_exit));
    }
    if (end <= 0xFEBCu || load > 0xFEC2u) {
        memcpy(m + 0xFEBC, irq_exit, sizeof(irq_exit));
    }
    if (load > 0x0315u || end <= 0x0314u) {
        m[0x314] = 0x31;
        m[0x315] = 0xEA;
    }
    int ntsc = ((h->flags >> 2) & 3u) == 2u;   /* 01 PAL, 10 NTSC, else PAL */
    if (load > 0x02A6u || end <= 0x02A6u) {
        m[0x2A6] = ntsc ? 0 : 1;                /* the KERNAL's PAL flag */
    }
    m[0] = 0x2F;
    /* PSID: banking by the init address; RSID: as the KERNAL leaves it */
    m[1] = h->rsid || init < 0xA000u ? 0x37 : init < 0xD000u ? 0x36 : init < 0xE000u ? 0x34 : 0x35;

    sidp.clock = ntsc ? 1022727u : 985248u;
    sidp.line_cycles = ntsc ? 65u : 63u;
    sidp.lines = ntsc ? 263u : 312u;
    io[0xC04] = ntsc ? 0x95 : 0x25;             /* the KERNAL's 60 Hz timer A latch */
    io[0xC05] = ntsc ? 0x42 : 0x40;
    sidp.fscale = ntsc ? 67025u : 64569u;       /* clock / 1 MHz, Q16 */
    sidp.upc = ntsc ? 256319u : 266069u;        /* 4e6 / clock, Q16 */
    sidp.cpu_q16 = ntsc ? 16756u : 16142u;      /* clock / 4e6, Q16 */
    unsigned bit = song <= 32 ? (unsigned)song - 1u : 31u;
    sidp.cia_speed = (h->speed >> bit) & 1u;
    sidp.cia_mask = 1u;                         /* KERNAL: timer A IRQ on */
    sidp.sid2 = (uint16_t)(h->sid2 << 4);
    sidp.sid3 = (uint16_t)(h->sid3 << 4);
    sidp.init = (uint16_t)init;
    sidp.play = h->rsid ? 0u : h->play;
    sidp.song = (uint8_t)song;
    sidp.rnd = 0x1234567u;
    return song;
}

/* Run init, schedule the first call. */
static void sidp_start(uint32_t now) {
    sidp_silence();
    sidp.call_t = sidp.frame0 = sidp.cia_last = now;
    /* An init that never returns (RSID main loops) is left after the budget;
     * the interrupt handler it installed is what plays the tune. Its writes
     * go out as they come: init may run for longer than any lead. */
    sidp.direct = 1;
    sidp_call(sidp.init, (uint8_t)(sidp.song - 1u), SIDP_INIT_BUDGET);
    sidp.direct = 0;
    sidp.due = now;
    sidp_schedule();
}

/* Call as often as possible; returns 1 once the tune is stuck (stop it). */
static int sidp_poll(uint32_t now) {
    sidp_flush(now);
    for (int n = 0; n < SIDP_MAX_AHEAD && !SIDP_AFTER(sidp.due, now + SIDP_LEAD); n++) {
        int r;
        sidp.call_t = sidp.due;
        sidp.ncalls[sidp.play ? 0 : sidp.next_src]++;
        if (sidp.play) {
            r = sidp_call(sidp.play, 0, SIDP_PLAY_BUDGET);
        } else {
            if (sidp.next_src == 1) {
                sidp.vic_irq = 0x81u;
            } else if (sidp.next_src == 2) {
                sidp.cia_icr = 0x81u;
                sidp.cia_last = sidp.due;
            }
            r = sidp_irq(SIDP_PLAY_BUDGET);
        }
        if (r == VX_RUN_TRAP) {
            sidp.fails = 0;
        } else if (++sidp.fails >= SIDP_MAX_FAILS) {
            return 1;
        }
        sidp_schedule();
    }
    uint32_t frame = sidp_units(sidp.line_cycles * sidp.lines);
    if (SIDP_AFTER(now, sidp.due + 4u * frame)) {
        /* the host blocked for a while: move the virtual clock up instead
         * of playing the missed calls in a burst */
        uint32_t d = now - sidp.due;
        sidp.due += d;
        sidp.frame0 += d;
        sidp.cia_last += d;
    }
    sidp_flush(now);
    return 0;
}

#endif /* VX_SIDPLAY_H */

/* SPDX-License-Identifier: MIT
 * Copyright (c) 2025-2026 Robert S. (aka Robmaster) */
/*
 * vx6502.h -- a compact NMOS 6502 core for the SID player (vixen_sound.c).
 *
 * Portable C, no MicroPython: the same file is built on the PC for the test
 * harness (tools/sidtest). Include it once, after defining
 *
 *   uint8_t vx_io_rd(uint16_t addr)            $D000-$DFFF reads with I/O in
 *   void    vx_io_wr(uint16_t addr, uint8_t v) $D000-$DFFF writes with I/O in
 *
 * Memory is a flat 64 KB RAM (VX6502.ram). The I/O area is mapped in exactly
 * when the C64's processor port ($01) maps it: CHAREN set and LORAM or HIRAM
 * set. ROMs are not emulated: a PSID tune only needs RAM, I/O and the few
 * KERNAL entry points the player puts in RAM itself.
 *
 * Documented opcodes, decimal mode and the stable undocumented opcodes that
 * SID players use (LAX, SAX, DCP, ISC, SLO, RLA, SRE, RRA, ANC, ALR, ARR,
 * SBX, the NOP variants). The unstable SHA/SHX/SHY/TAS/LAS group follows the
 * usual "& (high byte + 1)" model. BRK and the JAM opcodes stop the run.
 * Cycle counts are the base counts (no page-crossing extra) -- they only
 * bound a call's runtime, nothing is timed against them.
 */

#ifndef VX6502_H
#define VX6502_H

#include <stdint.h>

#define VX_C 0x01u
#define VX_Z 0x02u
#define VX_I 0x04u
#define VX_D 0x08u
#define VX_B 0x10u
#define VX_U 0x20u
#define VX_V 0x40u
#define VX_N 0x80u

typedef struct {
    uint8_t *ram;          /* 64 KB */
    uint16_t pc;
    uint8_t a, x, y, s, p;
    uint32_t cyc;          /* cycles of the current run, valid inside the I/O hooks */
} vx6502_t;

/* Stop reasons of vx6502_run(). */
#define VX_RUN_TRAP    0   /* PC reached the trap address: the call returned */
#define VX_RUN_BUDGET  1   /* cycle budget used up */
#define VX_RUN_HALT    2   /* BRK or a JAM opcode */

static uint8_t vx_io_rd(uint16_t addr);
static void vx_io_wr(uint16_t addr, uint8_t v);

static const uint8_t vx_cycles[256] = {
    7, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6,
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6,
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6,
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6,
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,
    2, 6, 2, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,
    2, 5, 2, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
};

/* I/O visible at $D000-$DFFF? (processor port in RAM at $01) */
#define VX_IO_IN(m) (((m)[1] & 4u) && ((m)[1] & 3u))

/* Run from c->pc until PC == trap, the budget is used up or BRK/JAM.
 * *used receives the cycles spent. */
static int vx6502_run(vx6502_t *c, uint16_t trap, uint32_t budget, uint32_t *used) {
    uint8_t *const m = c->ram;
    uint16_t pc = c->pc;
    uint8_t a = c->a, x = c->x, y = c->y, s = c->s, p = c->p;
    uint32_t cyc = 0;
    int why = VX_RUN_BUDGET;

    /* the I/O hooks see the cycle count (c->cyc): the player times with it */
    #define RD(ad) (((((ad) & 0xF000u) == 0xD000u) && VX_IO_IN(m)) ? (c->cyc = cyc, vx_io_rd(ad)) : m[(ad)])
    #define WR(ad, v) do { uint16_t w_ = (ad); \
                           if ((w_ & 0xF000u) == 0xD000u && VX_IO_IN(m)) { c->cyc = cyc; vx_io_wr(w_, (v)); } \
                           else { m[w_] = (v); } } while (0)
    #define NZ(v) (p = (uint8_t)((p & ~(VX_N | VX_Z)) | ((v) & 0x80u) | (((v) & 0xFFu) ? 0u : VX_Z)))
    #define PUSH(v) (m[0x100u + s--] = (uint8_t)(v))
    #define PULL() (m[0x100u + (uint8_t)++s])
    #define W16(ad) ((uint16_t)(m[(uint16_t)(ad)] | (m[(uint16_t)((ad) + 1u)] << 8)))
    /* effective addresses; the operand bytes are always fetched from RAM */
    #define EA_ZP()  (m[pc++])
    #define EA_ZPX() ((uint8_t)(m[pc++] + x))
    #define EA_ZPY() ((uint8_t)(m[pc++] + y))
    #define EA_ABS() (pc += 2u, W16(pc - 2u))
    #define EA_ABX() ((uint16_t)(EA_ABS() + x))
    #define EA_ABY() ((uint16_t)(EA_ABS() + y))
    #define EA_IZX() (t8 = (uint8_t)(m[pc++] + x), (uint16_t)(m[t8] | (m[(uint8_t)(t8 + 1u)] << 8)))
    #define EA_IZY() (t8 = m[pc++], (uint16_t)((m[t8] | (m[(uint8_t)(t8 + 1u)] << 8)) + y))
    #define BRANCH(cond) do { int8_t o_ = (int8_t)m[pc++]; \
                              if (cond) { pc = (uint16_t)(pc + o_); cyc++; } } while (0)

    while (cyc < budget) {
        if (pc == trap) {
            why = VX_RUN_TRAP;
            break;
        }
        uint8_t op = m[pc++];
        uint16_t ea;
        uint8_t v, t8;
        unsigned t;
        cyc += vx_cycles[op];

        /* addressing mode by the opcode's low bits, for the regular groups */
        switch (op) {
            /* ---- loads/stores ---- */
            case 0xA9: a = m[pc++]; NZ(a); continue;
            case 0xA5: a = m[EA_ZP()]; NZ(a); continue;
            case 0xB5: a = m[EA_ZPX()]; NZ(a); continue;
            case 0xAD: ea = EA_ABS(); a = RD(ea); NZ(a); continue;
            case 0xBD: ea = EA_ABX(); a = RD(ea); NZ(a); continue;
            case 0xB9: ea = EA_ABY(); a = RD(ea); NZ(a); continue;
            case 0xA1: ea = EA_IZX(); a = RD(ea); NZ(a); continue;
            case 0xB1: ea = EA_IZY(); a = RD(ea); NZ(a); continue;
            case 0xA2: x = m[pc++]; NZ(x); continue;
            case 0xA6: x = m[EA_ZP()]; NZ(x); continue;
            case 0xB6: x = m[EA_ZPY()]; NZ(x); continue;
            case 0xAE: ea = EA_ABS(); x = RD(ea); NZ(x); continue;
            case 0xBE: ea = EA_ABY(); x = RD(ea); NZ(x); continue;
            case 0xA0: y = m[pc++]; NZ(y); continue;
            case 0xA4: y = m[EA_ZP()]; NZ(y); continue;
            case 0xB4: y = m[EA_ZPX()]; NZ(y); continue;
            case 0xAC: ea = EA_ABS(); y = RD(ea); NZ(y); continue;
            case 0xBC: ea = EA_ABX(); y = RD(ea); NZ(y); continue;
            case 0x85: m[EA_ZP()] = a; continue;
            case 0x95: m[EA_ZPX()] = a; continue;
            case 0x8D: ea = EA_ABS(); WR(ea, a); continue;
            case 0x9D: ea = EA_ABX(); WR(ea, a); continue;
            case 0x99: ea = EA_ABY(); WR(ea, a); continue;
            case 0x81: ea = EA_IZX(); WR(ea, a); continue;
            case 0x91: ea = EA_IZY(); WR(ea, a); continue;
            case 0x86: m[EA_ZP()] = x; continue;
            case 0x96: m[EA_ZPY()] = x; continue;
            case 0x8E: ea = EA_ABS(); WR(ea, x); continue;
            case 0x84: m[EA_ZP()] = y; continue;
            case 0x94: m[EA_ZPX()] = y; continue;
            case 0x8C: ea = EA_ABS(); WR(ea, y); continue;

            /* ---- transfers, stack, flags ---- */
            case 0xAA: x = a; NZ(x); continue;
            case 0xA8: y = a; NZ(y); continue;
            case 0x8A: a = x; NZ(a); continue;
            case 0x98: a = y; NZ(a); continue;
            case 0xBA: x = s; NZ(x); continue;
            case 0x9A: s = x; continue;
            case 0x48: PUSH(a); continue;
            case 0x08: PUSH(p | VX_B | VX_U); continue;
            case 0x68: a = PULL(); NZ(a); continue;
            case 0x28: p = (uint8_t)((PULL() & ~VX_B) | VX_U); continue;
            case 0x18: p &= (uint8_t)~VX_C; continue;
            case 0x38: p |= VX_C; continue;
            case 0x58: p &= (uint8_t)~VX_I; continue;
            case 0x78: p |= VX_I; continue;
            case 0xB8: p &= (uint8_t)~VX_V; continue;
            case 0xD8: p &= (uint8_t)~VX_D; continue;
            case 0xF8: p |= VX_D; continue;

            /* ---- inc/dec registers ---- */
            case 0xE8: x++; NZ(x); continue;
            case 0xC8: y++; NZ(y); continue;
            case 0xCA: x--; NZ(x); continue;
            case 0x88: y--; NZ(y); continue;

            /* ---- jumps, branches ---- */
            case 0x4C: pc = W16(pc); continue;
            case 0x6C: ea = W16(pc);
                v = RD(ea);
                pc = (uint16_t)(v | (RD((uint16_t)((ea & 0xFF00u) | ((ea + 1u) & 0xFFu))) << 8));
                continue;
            case 0x20: ea = W16(pc); pc++;   /* pushes the address of its last byte */
                PUSH(pc >> 8); PUSH(pc & 0xFFu); pc = ea; continue;
            case 0x60: pc = PULL(); pc |= (uint16_t)(PULL() << 8); pc++; continue;
            case 0x40: p = (uint8_t)((PULL() & ~VX_B) | VX_U);
                pc = PULL(); pc |= (uint16_t)(PULL() << 8); continue;
            case 0x10: BRANCH(!(p & VX_N)); continue;
            case 0x30: BRANCH(p & VX_N); continue;
            case 0x50: BRANCH(!(p & VX_V)); continue;
            case 0x70: BRANCH(p & VX_V); continue;
            case 0x90: BRANCH(!(p & VX_C)); continue;
            case 0xB0: BRANCH(p & VX_C); continue;
            case 0xD0: BRANCH(!(p & VX_Z)); continue;
            case 0xF0: BRANCH(p & VX_Z); continue;

            /* ---- implied/immediate NOPs ---- */
            case 0xEA: case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA:
                continue;
            case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2:
            case 0x04: case 0x44: case 0x64:
            case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4:
                pc++; continue;
            case 0x0C: case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC:
                pc += 2u; continue;

            /* ---- accumulator shifts ---- */
            case 0x0A: p = (uint8_t)((p & ~VX_C) | (a >> 7)); a <<= 1; NZ(a); continue;
            case 0x4A: p = (uint8_t)((p & ~VX_C) | (a & 1u)); a >>= 1; NZ(a); continue;
            case 0x2A: t = (unsigned)(a << 1) | (p & VX_C); p = (uint8_t)((p & ~VX_C) | (a >> 7));
                a = (uint8_t)t; NZ(a); continue;
            case 0x6A: t = (unsigned)(a >> 1) | ((p & VX_C) << 7); p = (uint8_t)((p & ~VX_C) | (a & 1u));
                a = (uint8_t)t; NZ(a); continue;

            /* ---- immediate-only undocumented ---- */
            case 0x0B: case 0x2B: a &= m[pc++]; NZ(a); p = (uint8_t)((p & ~VX_C) | (a >> 7)); continue;
            case 0x4B: a &= m[pc++]; p = (uint8_t)((p & ~VX_C) | (a & 1u)); a >>= 1; NZ(a); continue;
            case 0x6B: a &= m[pc++]; a = (uint8_t)((a >> 1) | ((p & VX_C) << 7)); NZ(a);
                p = (uint8_t)((p & ~(VX_C | VX_V)) | ((a >> 6) & 1u) | (((a >> 6) ^ (a >> 5)) & 1u ? VX_V : 0u));
                continue;
            case 0xCB: v = m[pc++]; t = (unsigned)(a & x) - v;
                p = (uint8_t)((p & ~VX_C) | (t < 0x100u ? VX_C : 0u)); x = (uint8_t)t; NZ(x); continue;
            case 0xAB: a = x = (uint8_t)((a | 0xEEu) & m[pc++]); NZ(a); continue;
            case 0x8B: a = (uint8_t)((a | 0xEEu) & x & m[pc++]); NZ(a); continue;

            /* ---- unstable store group ---- */
            case 0x9C: ea = EA_ABX(); WR(ea, (uint8_t)(y & ((ea >> 8) + 1u))); continue;
            case 0x9E: ea = EA_ABY(); WR(ea, (uint8_t)(x & ((ea >> 8) + 1u))); continue;
            case 0x9F: ea = EA_ABY(); WR(ea, (uint8_t)(a & x & ((ea >> 8) + 1u))); continue;
            case 0x93: ea = EA_IZY(); WR(ea, (uint8_t)(a & x & ((ea >> 8) + 1u))); continue;
            case 0x9B: s = a & x; ea = EA_ABY(); WR(ea, (uint8_t)(s & ((ea >> 8) + 1u))); continue;
            case 0xBB: ea = EA_ABY(); a = x = s = (uint8_t)(RD(ea) & s); NZ(a); continue;

            /* ---- BRK, JAM: stop ---- */
            #ifdef VX6502_REAL_BRK   /* the PC test harness runs a real BRK */
            case 0x00:
                pc++;
                PUSH(pc >> 8); PUSH(pc & 0xFFu); PUSH(p | VX_B | VX_U);
                p |= VX_I;
                pc = W16(0xFFFEu);
                continue;
            #else
            case 0x00:
            #endif
            case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52:
            case 0x62: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2:
                pc--;
                why = VX_RUN_HALT;
                goto out;

            default:
                break;
        }

        /* The regular groups: ALU ops (cc=01), RMW ops (cc=10) and their
         * undocumented combinations (cc=11), compare/bit/inc/dec (cc=00). */
        switch (op & 0x1Fu) {
            /* operand address for column 1 / 3 (ALU / combos) */
            case 0x01: case 0x03: ea = EA_IZX(); break;
            case 0x05: case 0x07: case 0x04: case 0x06: ea = EA_ZP(); break;
            case 0x09: case 0x0B: ea = pc++; break;
            case 0x0D: case 0x0F: case 0x0C: case 0x0E: ea = EA_ABS(); break;
            case 0x11: case 0x13: ea = EA_IZY(); break;
            case 0x15: case 0x14: ea = EA_ZPX(); break;
            case 0x16: case 0x17: ea = ((op & 0xC0u) == 0x80u) ? EA_ZPY() : EA_ZPX(); break;
            case 0x19: case 0x1B: ea = EA_ABY(); break;
            case 0x1D: case 0x1C: ea = EA_ABX(); break;
            case 0x1E: case 0x1F: ea = ((op & 0xC0u) == 0x80u) ? EA_ABY() : EA_ABX(); break;
            case 0x00: case 0x02: case 0x08: case 0x0A: case 0x10: case 0x12: case 0x18: case 0x1A:
            default:
                ea = pc++;   /* immediate (CPX/CPY/LDX #) */
                break;
        }

        /* read-modify-write (no dummy write: nothing here reacts to it), SAX */
        switch (op) {
            /* ASL / SLO */
            case 0x06: case 0x16: case 0x0E: case 0x1E:
            case 0x07: case 0x17: case 0x0F: case 0x1F: case 0x1B: case 0x03: case 0x13:
                v = RD(ea);
                p = (uint8_t)((p & ~VX_C) | (v >> 7)); v <<= 1; WR(ea, v);
                if (op & 1u) { a |= v; NZ(a); } else { NZ(v); }
                continue;
            /* ROL / RLA */
            case 0x26: case 0x36: case 0x2E: case 0x3E:
            case 0x27: case 0x37: case 0x2F: case 0x3F: case 0x3B: case 0x23: case 0x33:
                v = RD(ea);
                t = (unsigned)(v << 1) | (p & VX_C); p = (uint8_t)((p & ~VX_C) | (v >> 7));
                v = (uint8_t)t; WR(ea, v);
                if (op & 1u) { a &= v; NZ(a); } else { NZ(v); }
                continue;
            /* LSR / SRE */
            case 0x46: case 0x56: case 0x4E: case 0x5E:
            case 0x47: case 0x57: case 0x4F: case 0x5F: case 0x5B: case 0x43: case 0x53:
                v = RD(ea);
                p = (uint8_t)((p & ~VX_C) | (v & 1u)); v >>= 1; WR(ea, v);
                if (op & 1u) { a ^= v; NZ(a); } else { NZ(v); }
                continue;
            /* ROR / RRA */
            case 0x66: case 0x76: case 0x6E: case 0x7E:
            case 0x67: case 0x77: case 0x6F: case 0x7F: case 0x7B: case 0x63: case 0x73:
                v = RD(ea);
                t = (unsigned)(v >> 1) | ((p & VX_C) << 7); p = (uint8_t)((p & ~VX_C) | (v & 1u));
                v = (uint8_t)t; WR(ea, v);
                if (op & 1u) { goto do_adc; }
                NZ(v);
                continue;
            /* DEC / DCP */
            case 0xC6: case 0xD6: case 0xCE: case 0xDE:
            case 0xC7: case 0xD7: case 0xCF: case 0xDF: case 0xDB: case 0xC3: case 0xD3:
                v = RD(ea); v--; WR(ea, v);
                if (op & 1u) { t = (unsigned)a - v; p = (uint8_t)((p & ~VX_C) | (a >= v ? VX_C : 0u)); NZ(t); }
                else { NZ(v); }
                continue;
            /* INC / ISC */
            case 0xE6: case 0xF6: case 0xEE: case 0xFE:
            case 0xE7: case 0xF7: case 0xEF: case 0xFF: case 0xFB: case 0xE3: case 0xF3:
                v = RD(ea); v++; WR(ea, v);
                if (op & 1u) { goto do_sbc; }
                NZ(v);
                continue;
            /* SAX */
            case 0x87: case 0x97: case 0x8F: case 0x83:
                WR(ea, (uint8_t)(a & x)); continue;
            default:
                break;
        }

        /* the rest read one operand */
        v = (ea < 0x100u) ? m[ea] : RD(ea);
        switch (op) {
            /* ORA, AND, EOR */
            case 0x01: case 0x05: case 0x09: case 0x0D: case 0x11: case 0x15: case 0x19: case 0x1D:
                a |= v; NZ(a); continue;
            case 0x21: case 0x25: case 0x29: case 0x2D: case 0x31: case 0x35: case 0x39: case 0x3D:
                a &= v; NZ(a); continue;
            case 0x41: case 0x45: case 0x49: case 0x4D: case 0x51: case 0x55: case 0x59: case 0x5D:
                a ^= v; NZ(a); continue;
            /* ADC */
            case 0x61: case 0x65: case 0x69: case 0x6D: case 0x71: case 0x75: case 0x79: case 0x7D:
            do_adc:
                if (p & VX_D) {
                    unsigned lo = (a & 0x0Fu) + (v & 0x0Fu) + (p & VX_C);
                    if (lo > 9u) {
                        lo += 6u;
                    }
                    unsigned hi = (unsigned)(a >> 4) + (v >> 4) + (lo > 0x0Fu);
                    t = (unsigned)a + v + (p & VX_C);
                    p = (uint8_t)(p & ~(VX_N | VX_Z | VX_V | VX_C));
                    if (!(t & 0xFFu)) {
                        p |= VX_Z;
                    }
                    if (hi & 8u) {
                        p |= VX_N;
                    }
                    if ((((hi << 4) ^ a) & 0x80u) && !((a ^ v) & 0x80u)) {
                        p |= VX_V;
                    }
                    if (hi > 9u) {
                        hi += 6u;
                    }
                    if (hi > 0x0Fu) {
                        p |= VX_C;
                    }
                    a = (uint8_t)((hi << 4) | (lo & 0x0Fu));
                } else {
                    t = (unsigned)a + v + (p & VX_C);
                    p = (uint8_t)((p & ~(VX_C | VX_V)) | (t > 0xFFu ? VX_C : 0u)
                        | ((~(a ^ v) & (a ^ t) & 0x80u) ? VX_V : 0u));
                    a = (uint8_t)t;
                    NZ(a);
                }
                continue;
            /* SBC (EB = undocumented copy) */
            case 0xE1: case 0xE5: case 0xE9: case 0xED: case 0xF1: case 0xF5: case 0xF9: case 0xFD:
            case 0xEB:
            do_sbc: {
                unsigned bc = (p & VX_C) ? 0u : 1u;
                t = (unsigned)a - v - bc;
                uint8_t np = (uint8_t)(p & ~(VX_C | VX_V | VX_N | VX_Z));
                if (t < 0x100u) {
                    np |= VX_C;
                }
                if ((a ^ v) & (a ^ t) & 0x80u) {
                    np |= VX_V;
                }
                np |= (uint8_t)(t & 0x80u);
                if (!(t & 0xFFu)) {
                    np |= VX_Z;
                }
                if (p & VX_D) {
                    unsigned lo = (a & 0x0Fu) - (v & 0x0Fu) - bc;
                    unsigned hi = (unsigned)(a >> 4) - (v >> 4);
                    if (lo & 0x10u) {
                        lo -= 6u;
                        hi--;
                    }
                    if (hi & 0x10u) {
                        hi -= 6u;
                    }
                    a = (uint8_t)((hi << 4) | (lo & 0x0Fu));
                } else {
                    a = (uint8_t)t;
                }
                p = np;
                continue;
            }
            /* CMP, CPX, CPY */
            case 0xC1: case 0xC5: case 0xC9: case 0xCD: case 0xD1: case 0xD5: case 0xD9: case 0xDD:
                t = (unsigned)a - v; p = (uint8_t)((p & ~VX_C) | (a >= v ? VX_C : 0u)); NZ(t); continue;
            case 0xE0: case 0xE4: case 0xEC:
                t = (unsigned)x - v; p = (uint8_t)((p & ~VX_C) | (x >= v ? VX_C : 0u)); NZ(t); continue;
            case 0xC0: case 0xC4: case 0xCC:
                t = (unsigned)y - v; p = (uint8_t)((p & ~VX_C) | (y >= v ? VX_C : 0u)); NZ(t); continue;
            /* BIT */
            case 0x24: case 0x2C:
                p = (uint8_t)((p & ~(VX_N | VX_V | VX_Z)) | (v & 0xC0u) | ((a & v) ? 0u : VX_Z)); continue;
            /* LAX */
            case 0xA7: case 0xB7: case 0xAF: case 0xBF: case 0xA3: case 0xB3:
                a = x = v; NZ(a); continue;
            default:
                break;
        }
        /* unreachable for valid tables; treat as a halt */
        pc--;
        why = VX_RUN_HALT;
        goto out;
    }
out:
    c->pc = pc;
    c->a = a; c->x = x; c->y = y; c->s = s; c->p = p;
    *used = cyc;
    return why;

    #undef RD
    #undef WR
    #undef NZ
    #undef PUSH
    #undef PULL
    #undef W16
    #undef EA_ZP
    #undef EA_ZPX
    #undef EA_ZPY
    #undef EA_ABS
    #undef EA_ABX
    #undef EA_ABY
    #undef EA_IZX
    #undef EA_IZY
    #undef BRANCH
}

#endif /* VX6502_H */

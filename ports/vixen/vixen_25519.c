/* SPDX-License-Identifier: MIT
 * Copyright (c) 2025-2026 Robert S. (aka Robmaster) */
/*
 * Module curve25519: X25519 key exchange (RFC 7748) and Ed25519 signatures
 * (RFC 8032) -- what an SSH server needs for "curve25519-sha256" and
 * "ssh-ed25519". mbedtls 3.6 has X25519 but no Ed25519, so both live here,
 * on one small field implementation.
 *
 *   curve25519.x25519(scalar32[, point32]) -> 32 bytes
 *       without point: the public key for that private scalar (base point 9)
 *       with point:    the shared secret; ValueError if it is all zero
 *   curve25519.ed25519_public(seed32) -> public key (32)
 *   curve25519.ed25519_sign(seed32, msg[, public32]) -> signature (64)
 *       passing the public key saves one scalar multiplication
 *   curve25519.ed25519_verify(public32, msg, sig64) -> bool
 *
 * Field GF(2^255-19) in radix 2^25.5 (ten signed limbs of 26/25 bits, as in
 * ref10/curve25519-donna): a multiply is 100 32x32->64 bit products (rv32
 * mul + mulh). Constant time throughout for secret data: Montgomery ladder for
 * X25519, a conditional-swap ladder for Ed25519 scalar multiplication, no
 * secret-dependent branches or table indices. Ed25519 point formulas:
 * extended twisted Edwards coordinates (Hisil-Wong-Carter-Dawson 2008),
 * unified addition and a dedicated doubling. Decoding, S < L check (RFC 8032
 * 5.1.7) and the scalar arithmetic mod L follow TweetNaCl.
 *
 * Speed on the VIXEN: the C stack lives in chip RAM, where every store is a
 * write-through to the SDRAM (tens of cycles). The scalar multiplications
 * therefore run on a small stack of their own in BRAM (c25519_stack below,
 * switched to by c25519_call); fe_mul/fe_sq are linked into BRAM too
 * (vixen_net.ld / vixen_lvgl.ld). The hashing and mod-L arithmetic run
 * once per call on the normal stack. Every ladder step also polls the raster
 * timebase, so time.ticks_ms() stays right across a long call.
 *
 * Private keys: 32 bytes from os.urandom (see vixen_random.c).
 */

#include <string.h>
#include <stdint.h>
#include "py/runtime.h"
#include "mbedtls/sha512.h"

void ticks_poll(void);

typedef int32_t fe[10];
typedef uint8_t u8;

/* ---- field arithmetic ---------------------------------------------------- */

/* Limb i holds bits [off_i, off_i + w_i) with w = 26, 25, 26, 25, ...;
 * 2^255 = 19 (mod p). Carried limbs are |h_i| <= 2^25 (even) / 2^24 (odd);
 * sums and differences are not carried. fe_mul/fe_sq accept limbs up to
 * 2^27 / 2^26 (four carried values added): the largest coefficient sum is
 * then < 181 * 2^54 < 2^63. */

static const fe fe_one = {1};
static const fe fe_d = {0x35978a3, 0xd37284, 0x3156ebd, 0x6a0a0e, 0x1c029, 0x179e898, 0x3a03cbb, 0x1ce7198, 0x2e2b6ff, 0x1480db3};
static const fe fe_d2 = {0x2b2f159, 0x1a6e509, 0x22add7a, 0xd4141d, 0x38052, 0xf3d130, 0x3407977, 0x19ce331, 0x1c56dff, 0x901b67};
static const fe fe_sqrtm1 = {0x20ea0b0, 0x186c9d2, 0x8f189d, 0x35697f, 0xbd0c60, 0x1fbd7a7, 0x2804c9e, 0x1e16569, 0x4fc1d, 0xae0c92};
static const fe fe_bx = {0x325d51a, 0x18b5823, 0xf6592a, 0x104a92d, 0x1a4b31d, 0x1d6dc5c, 0x27118fe, 0x7fd814, 0x13cd6e5, 0x85a4db};
static const fe fe_by = {0x2666658, 0x1999999, 0xcccccc, 0x1333333, 0x1999999, 0x666666, 0x3333333, 0xcccccc, 0x2666666, 0x1999999};

#define W(i) (26 - ((i) & 1))

static void fe_copy(fe h, const fe f) {
    memcpy(h, f, sizeof(fe));
}

static void fe_add(fe h, const fe f, const fe g) {
    for (int i = 0; i < 10; ++i) {
        h[i] = f[i] + g[i];
    }
}

static void fe_sub(fe h, const fe f, const fe g) {
    for (int i = 0; i < 10; ++i) {
        h[i] = f[i] - g[i];
    }
}

static void fe_neg(fe h, const fe f) {
    for (int i = 0; i < 10; ++i) {
        h[i] = -f[i];
    }
}

/* swap f and g when b == 1, in constant time */
static void fe_cswap(fe f, fe g, int b) {
    int32_t m = -(int32_t)b;
    for (int i = 0; i < 10; ++i) {
        int32_t t = m & (f[i] ^ g[i]);
        f[i] ^= t;
        g[i] ^= t;
    }
}

/* 64-bit coefficients -> carried limbs (rounding carries, signed limbs) */
static inline __attribute__((always_inline)) void fe_carry64(fe h, int64_t *t) {
    int64_t c;
    #pragma GCC unroll 10
    for (int i = 0; i < 9; ++i) {
        c = (t[i] + ((int64_t)1 << (W(i) - 1))) >> W(i);
        t[i + 1] += c;
        t[i] -= c << W(i);
    }
    c = (t[9] + ((int64_t)1 << 24)) >> 25;
    t[0] += c * 19;
    t[9] -= c << 25;
    c = (t[0] + ((int64_t)1 << 25)) >> 26;
    t[1] += c;
    t[0] -= c << 26;
    #pragma GCC unroll 10
    for (int i = 0; i < 10; ++i) {
        h[i] = (int32_t)t[i];
    }
}

/* h = f * g. Coefficient k collects f_i g_j with i + j = k, plus 19 times
 * those with i + j = k + 10; a product of two odd limbs counts twice (their
 * offsets add up to one bit more than the target limb's). */
__attribute__((noinline)) static void fe_mul(fe h, const fe f, const fe g) {
    int32_t f2[10];
    int64_t t[10];
    #pragma GCC unroll 10
    for (int i = 0; i < 10; ++i) {
        f2[i] = (i & 1) ? 2 * f[i] : f[i];
    }
    #pragma GCC unroll 10
    for (int k = 0; k < 10; ++k) {
        int64_t lo = 0, hi = 0;
        #pragma GCC unroll 10
        for (int i = 0; i <= k; ++i) {
            int j = k - i;
            lo += (int64_t)((i & j & 1) ? f2[i] : f[i]) * g[j];
        }
        #pragma GCC unroll 10
        for (int i = k + 1; i < 10; ++i) {
            int j = k + 10 - i;
            hi += (int64_t)((i & j & 1) ? f2[i] : f[i]) * g[j];
        }
        t[k] = lo + hi * 19;
    }
    fe_carry64(h, t);
}

/* h = f^2: fe_mul with the symmetric products folded */
__attribute__((noinline)) static void fe_sq(fe h, const fe f) {
    int32_t f2[10];
    int64_t t[10];
    #pragma GCC unroll 10
    for (int i = 0; i < 10; ++i) {
        f2[i] = 2 * f[i];
    }
    #pragma GCC unroll 10
    for (int k = 0; k < 10; ++k) {
        int64_t lo = 0, hi = 0;
        #pragma GCC unroll 10
        for (int i = 0; 2 * i <= k; ++i) {
            int j = k - i;
            int dbl = (i != j) + (i & j & 1);   /* 0: once, 1: twice, 2: four times */
            int64_t p = (int64_t)(dbl ? f2[i] : f[i]) * f[j];
            lo += dbl == 2 ? 2 * p : p;
        }
        #pragma GCC unroll 10
        for (int i = k + 1; 2 * i <= k + 10; ++i) {
            int j = k + 10 - i;
            int dbl = (i != j) + (i & j & 1);
            int64_t p = (int64_t)(dbl ? f2[i] : f[i]) * f[j];
            hi += dbl == 2 ? 2 * p : p;
        }
        t[k] = lo + hi * 19;
    }
    fe_carry64(h, t);
}

/* h = f * n for a small constant n (< 2^17) */
static void fe_mul_small(fe h, const fe f, int32_t n) {
    int64_t t[10];
    for (int i = 0; i < 10; ++i) {
        t[i] = (int64_t)f[i] * n;
    }
    fe_carry64(h, t);
}

static void fe_sqn(fe h, const fe f, int n) {
    fe_sq(h, f);
    while (--n) {
        fe_sq(h, h);
    }
}

/* z^(2^250 - 1) and z^11, the common part of inversion and square root */
__attribute__((noinline)) static void fe_pow250(fe z2_250_0, fe z11, const fe z) {
    fe z2, z9, a, b, c;
    fe_sq(z2, z);                 /* 2 */
    fe_sqn(a, z2, 2);             /* 8 */
    fe_mul(z9, a, z);             /* 9 */
    fe_mul(z11, z9, z2);          /* 11 */
    fe_sq(a, z11);                /* 22 */
    fe_mul(a, a, z9);             /* 2^5 - 1 */
    fe_sqn(b, a, 5);
    fe_mul(a, b, a);              /* 2^10 - 1 */
    fe_sqn(b, a, 10);
    fe_mul(b, b, a);              /* 2^20 - 1 */
    fe_sqn(c, b, 20);
    fe_mul(c, c, b);              /* 2^40 - 1 */
    fe_sqn(c, c, 10);
    fe_mul(b, c, a);              /* 2^50 - 1 */
    fe_sqn(a, b, 50);
    fe_mul(a, a, b);              /* 2^100 - 1 */
    fe_sqn(c, a, 100);
    fe_mul(c, c, a);              /* 2^200 - 1 */
    fe_sqn(c, c, 50);
    fe_mul(z2_250_0, c, b);       /* 2^250 - 1 */
}

/* h = z^(p-2) = 1/z */
__attribute__((noinline)) static void fe_invert(fe h, const fe z) {
    fe t, z11;
    fe_pow250(t, z11, z);
    fe_sqn(t, t, 5);
    fe_mul(h, t, z11);            /* 2^255 - 21 */
}

/* h = z^((p-5)/8) = z^(2^252 - 3) */
__attribute__((noinline)) static void fe_pow2523(fe h, const fe z) {
    fe t, z11;
    fe_pow250(t, z11, z);
    fe_sqn(t, t, 2);
    fe_mul(h, t, z);
}

/* 32 little-endian bytes -> field element; bit 255 is ignored */
static void fe_frombytes(fe h, const u8 *s) {
    int off = 0;
    for (int i = 0; i < 10; ++i) {
        int b = off >> 3;
        uint64_t v = 0;
        for (int k = 0; k < 5 && b + k < 32; ++k) {
            v |= (uint64_t)s[b + k] << (8 * k);
        }
        h[i] = (int32_t)((v >> (off & 7)) & ((1u << W(i)) - 1));
        off += W(i);
    }
}

/* canonical encoding (0 <= value < p), constant time */
__attribute__((noinline)) static void fe_tobytes(u8 *s, const fe f) {
    int32_t h[10], r[10];
    fe_copy(h, f);
    /* floor carries: limbs into [0, 2^w), the top carry folded back (x19);
     * three passes settle any carried or lightly summed input, leaving
     * 0 <= value < 2^255 */
    for (int pass = 0; pass < 3; ++pass) {
        for (int i = 0; i < 9; ++i) {
            int32_t c = h[i] >> W(i);
            h[i + 1] += c;
            h[i] -= c * (1 << W(i));
        }
        int32_t c = h[9] >> 25;
        h[0] += c * 19;
        h[9] -= c * (1 << 25);
    }
    /* r = value - p; keep it if it did not borrow */
    int32_t borrow = 0;
    for (int i = 0; i < 10; ++i) {
        int32_t pl = (i == 0) ? 0x3ffffed : ((1 << W(i)) - 1);
        int32_t v = h[i] - pl - borrow;
        borrow = (v >> 31) & 1;
        r[i] = v + (borrow << W(i));
    }
    int32_t keep = borrow - 1;          /* all ones when r >= 0 */
    for (int i = 0; i < 10; ++i) {
        h[i] ^= keep & (h[i] ^ r[i]);
    }
    uint64_t acc = 0;
    int bits = 0, n = 0;
    for (int i = 0; i < 10; ++i) {
        acc |= (uint64_t)(uint32_t)h[i] << bits;
        bits += W(i);
        while (bits >= 8) {
            s[n++] = (u8)acc;
            acc >>= 8;
            bits -= 8;
        }
    }
    s[n] = (u8)acc;                     /* bits 248..254 */
}

static int fe_isodd(const fe f) {
    u8 s[32];
    fe_tobytes(s, f);
    return s[0] & 1;
}

static int fe_neq(const fe f, const fe g) {
    u8 a[32], b[32];
    fe_tobytes(a, f);
    fe_tobytes(b, g);
    u8 x = 0;
    for (int i = 0; i < 32; ++i) {
        x |= a[i] ^ b[i];
    }
    return x != 0;
}

/* ---- X25519 -------------------------------------------------------------- */

typedef struct {
    u8 out[32];
    u8 scalar[32];
    u8 point[32];
} x25519_job;

static void x25519_run(void *arg) {
    x25519_job *job = arg;
    u8 *z = job->scalar;
    fe x1, x2, z2, x3, z3, a, b, c, d;
    z[31] = (z[31] & 127) | 64;
    z[0] &= 248;
    fe_frombytes(x1, job->point);
    fe_copy(x2, fe_one);
    memset(z2, 0, sizeof(fe));
    fe_copy(x3, x1);
    fe_copy(z3, fe_one);
    int swap = 0;
    for (int i = 254; i >= 0; --i) {
        int bit = (z[i >> 3] >> (i & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;
        fe_add(a, x2, z2);          /* A */
        fe_sub(b, x2, z2);          /* B */
        fe_add(c, x3, z3);          /* C */
        fe_sub(d, x3, z3);          /* D */
        fe_mul(d, d, a);            /* DA */
        fe_mul(c, c, b);            /* CB */
        fe_sq(a, a);                /* AA */
        fe_sq(b, b);                /* BB */
        fe_add(x3, d, c);
        fe_sq(x3, x3);              /* x3 = (DA + CB)^2 */
        fe_sub(z3, d, c);
        fe_sq(z3, z3);
        fe_mul(z3, z3, x1);         /* z3 = x1 (DA - CB)^2 */
        fe_mul(x2, a, b);           /* x2 = AA BB */
        fe_sub(b, a, b);            /* E = AA - BB */
        fe_mul_small(c, b, 121665);
        fe_add(c, c, a);
        fe_mul(z2, b, c);           /* z2 = E (AA + a24 E) */
        ticks_poll();
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);
    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_tobytes(job->out, x2);
}

/* ---- Ed25519 points ------------------------------------------------------ */

typedef struct {
    fe X, Y, Z, T;
} ge;

static void ge_identity(ge *p) {
    memset(p, 0, sizeof(*p));
    p->Y[0] = 1;
    p->Z[0] = 1;
}

static void ge_base(ge *p) {
    fe_copy(p->X, fe_bx);
    fe_copy(p->Y, fe_by);
    fe_copy(p->Z, fe_one);
    fe_mul(p->T, fe_bx, fe_by);
}

/* p += q (unified, add-2008-hwcd-3 with k = 2d) */
__attribute__((noinline)) static void ge_add(ge *p, const ge *q) {
    fe a, b, c, d, t;
    fe_sub(a, p->Y, p->X);
    fe_sub(t, q->Y, q->X);
    fe_mul(a, a, t);
    fe_add(b, p->Y, p->X);
    fe_add(t, q->Y, q->X);
    fe_mul(b, b, t);
    fe_mul(c, p->T, q->T);
    fe_mul(c, c, fe_d2);
    fe_mul(d, p->Z, q->Z);
    fe_add(d, d, d);
    fe_sub(t, b, a);            /* E */
    fe_add(b, b, a);            /* H */
    fe_add(a, d, c);            /* G */
    fe_sub(d, d, c);            /* F */
    fe_mul(p->X, t, d);
    fe_mul(p->Y, a, b);
    fe_mul(p->Z, d, a);
    fe_mul(p->T, t, b);
}

/* p = 2p (dbl-2008-hwcd, a = -1) */
__attribute__((noinline)) static void ge_dbl(ge *p) {
    fe a, b, c, e, g;
    fe_sq(a, p->X);             /* A */
    fe_sq(b, p->Y);             /* B */
    fe_sq(c, p->Z);
    fe_add(c, c, c);            /* C */
    fe_add(e, p->X, p->Y);
    fe_sq(e, e);
    fe_sub(e, e, a);
    fe_sub(e, e, b);            /* E = (X+Y)^2 - A - B */
    fe_sub(g, b, a);            /* G = B - A */
    fe_add(b, a, b);            /* -H = A + B */
    fe_sub(c, g, c);            /* F = G - C */
    fe_mul(p->X, e, c);         /* E F */
    fe_mul(p->Y, g, b);         /* G H = -G (A + B); with H negated: */
    fe_neg(p->Y, p->Y);
    fe_mul(p->Z, c, g);         /* F G */
    fe_mul(p->T, e, b);
    fe_neg(p->T, p->T);         /* E H */
}

static void ge_cswap(ge *p, ge *q, int b) {
    fe_cswap(p->X, q->X, b);
    fe_cswap(p->Y, q->Y, b);
    fe_cswap(p->Z, q->Z, b);
    fe_cswap(p->T, q->T, b);
}

/* p = s * q (q is clobbered), constant time */
__attribute__((noinline)) static void ge_scalarmult(ge *p, ge *q, const u8 *s) {
    ge_identity(p);
    for (int i = 255; i >= 0; --i) {
        int b = (s[i >> 3] >> (i & 7)) & 1;
        ge_cswap(p, q, b);
        ge_add(q, p);
        ge_dbl(p);
        ge_cswap(p, q, b);
        ticks_poll();
    }
}

__attribute__((noinline)) static void ge_scalarbase(ge *p, const u8 *s) {
    ge q;
    ge_base(&q);
    ge_scalarmult(p, &q, s);
}

__attribute__((noinline)) static void ge_tobytes(u8 *r, const ge *p) {
    fe zi, x, y;
    fe_invert(zi, p->Z);
    fe_mul(x, p->X, zi);
    fe_mul(y, p->Y, zi);
    fe_tobytes(r, y);
    r[31] ^= fe_isodd(x) << 7;
}

/* decode a public key and negate it (TweetNaCl's unpackneg) */
__attribute__((noinline)) static int ge_frombytes_neg(ge *r, const u8 *p) {
    fe t, chk, num, den, den2, den4, den6;
    fe_copy(r->Z, fe_one);
    fe_frombytes(r->Y, p);
    fe_sq(num, r->Y);
    fe_mul(den, num, fe_d);
    fe_sub(num, num, r->Z);
    fe_add(den, r->Z, den);
    fe_sq(den2, den);
    fe_sq(den4, den2);
    fe_mul(den6, den4, den2);
    fe_mul(t, den6, num);
    fe_mul(t, t, den);
    fe_pow2523(t, t);
    fe_mul(t, t, num);
    fe_mul(t, t, den);
    fe_mul(t, t, den);
    fe_mul(r->X, t, den);
    fe_sq(chk, r->X);
    fe_mul(chk, chk, den);
    if (fe_neq(chk, num)) {
        fe_mul(r->X, r->X, fe_sqrtm1);
    }
    fe_sq(chk, r->X);
    fe_mul(chk, chk, den);
    if (fe_neq(chk, num)) {
        return -1;
    }
    if (fe_isodd(r->X) == (p[31] >> 7)) {
        fe_neg(r->X, r->X);
    }
    fe_mul(r->T, r->X, r->Y);
    return 0;
}

/* ---- the BRAM stack ------------------------------------------------------ */

/* 1.5 KB, ~1 KB used at the peak (vxemu.py reports it). Not zeroed at boot;
 * secrets left in it are overwritten by the next call and reachable only
 * from here. */
#define C25519_STACK_WORDS 384
#define C25519_CANARY 0xC25519CAu
static uint32_t c25519_stack[C25519_STACK_WORDS] __attribute__((section(".bram_bss"), aligned(16)));

/* c25519_call(fn, arg, stack_top): fn(arg) with sp = stack_top */
void c25519_call(void (*fn)(void *), void *arg, void *top);
__asm__(
    ".pushsection .text.c25519_call,\"ax\",@progbits\n"
    ".globl c25519_call\n"
    ".type c25519_call,@function\n"
    "c25519_call:\n"
    "    addi a2, a2, -16\n"
    "    sw   ra, 12(a2)\n"
    "    sw   sp, 8(a2)\n"
    "    mv   sp, a2\n"
    "    mv   t1, a0\n"
    "    mv   a0, a1\n"
    "    jalr t1\n"
    "    lw   ra, 12(sp)\n"
    "    lw   sp, 8(sp)\n"
    "    ret\n"
    ".size c25519_call, .-c25519_call\n"
    ".popsection\n"
    );

/* The lowest words are a canary: below the stack lies the BRAM .sbss. */
static void on_bram_stack(void (*fn)(void *), void *arg) {
    for (int i = 0; i < 4; ++i) {
        c25519_stack[i] = C25519_CANARY;
    }
    c25519_call(fn, arg, &c25519_stack[C25519_STACK_WORDS]);
    for (int i = 0; i < 4; ++i) {
        if (c25519_stack[i] != C25519_CANARY) {
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("curve25519 stack overflow"));
        }
    }
}

/* ---- AES-CTR, AES-GCM and SHA-256 on the same BRAM stack -----------------
 *
 * mbedtls keeps its round state in locals; on the chip-RAM stack every one of
 * those spills is a write-through store. Measured on the hardware: AES-CTR
 * ~140 KB/s and SHA-256 ~240 KB/s -- the limit for sshd (aes-ctr + hmac) and
 * for TLS. The linker (-Wl,--wrap, see the Makefile) sends every call of
 * these entry points through here; they run on c25519_stack. A call that is
 * already on it (nested) goes straight through. */
#include "mbedtls/aes.h"
#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"

static inline bool sp_on_bram_stack(void) {
    uintptr_t sp;
    __asm__ volatile ("mv %0, sp" : "=r" (sp));
    return sp > (uintptr_t)c25519_stack && sp <= (uintptr_t)&c25519_stack[C25519_STACK_WORDS];
}

typedef struct {
    void *ctx;
    size_t len;
    void *p1, *p2, *p3;
    const unsigned char *in;
    unsigned char *out;
    int ret;
} bram_args_t;

int __real_mbedtls_aes_crypt_ctr(mbedtls_aes_context *ctx, size_t length, size_t *nc_off,
    unsigned char nonce_counter[16], unsigned char stream_block[16],
    const unsigned char *input, unsigned char *output);
int __real_mbedtls_gcm_update(mbedtls_gcm_context *ctx, const unsigned char *input, size_t input_length,
    unsigned char *output, size_t output_size, size_t *output_length);
int __real_mbedtls_sha256_update(mbedtls_sha256_context *ctx, const unsigned char *input, size_t ilen);
int __real_mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char *output);

/* The counter and keystream block live in the caller's object (chip RAM) and
 * are rewritten every 16 bytes: work on BRAM copies, write them back once. */
static void aes_ctr_on_bram(void *p) {
    bram_args_t *a = p;
    uint32_t nc[4], sb[4];
    size_t off = *(size_t *)a->p1;
    memcpy(nc, a->p2, 16);
    memcpy(sb, a->p3, 16);
    a->ret = __real_mbedtls_aes_crypt_ctr(a->ctx, a->len, &off, (unsigned char *)nc,
        (unsigned char *)sb, a->in, a->out);
    *(size_t *)a->p1 = off;
    memcpy(a->p2, nc, 16);
    memcpy(a->p3, sb, 16);
}

int __wrap_mbedtls_aes_crypt_ctr(mbedtls_aes_context *ctx, size_t length, size_t *nc_off,
    unsigned char nonce_counter[16], unsigned char stream_block[16],
    const unsigned char *input, unsigned char *output) {
    if (sp_on_bram_stack()) {
        return __real_mbedtls_aes_crypt_ctr(ctx, length, nc_off, nonce_counter, stream_block, input, output);
    }
    bram_args_t a = { ctx, length, nc_off, nonce_counter, stream_block, input, output, 0 };
    on_bram_stack(aes_ctr_on_bram, &a);
    return a.ret;
}

static void gcm_update_on_bram(void *p) {
    bram_args_t *a = p;
    a->ret = __real_mbedtls_gcm_update(a->ctx, a->in, a->len, a->out, (size_t)a->p1, a->p2);
}

int __wrap_mbedtls_gcm_update(mbedtls_gcm_context *ctx, const unsigned char *input, size_t input_length,
    unsigned char *output, size_t output_size, size_t *output_length) {
    if (sp_on_bram_stack()) {
        return __real_mbedtls_gcm_update(ctx, input, input_length, output, output_size, output_length);
    }
    bram_args_t a = { ctx, input_length, (void *)output_size, output_length, NULL, input, output, 0 };
    on_bram_stack(gcm_update_on_bram, &a);
    return a.ret;
}

static void sha256_update_on_bram(void *p) {
    bram_args_t *a = p;
    a->ret = __real_mbedtls_sha256_update(a->ctx, a->in, a->len);
}

int __wrap_mbedtls_sha256_update(mbedtls_sha256_context *ctx, const unsigned char *input, size_t ilen) {
    if (ilen < 64 || sp_on_bram_stack()) {     /* no full block: nothing to gain */
        return __real_mbedtls_sha256_update(ctx, input, ilen);
    }
    bram_args_t a = { ctx, ilen, NULL, NULL, NULL, input, NULL, 0 };
    on_bram_stack(sha256_update_on_bram, &a);
    return a.ret;
}

static void sha256_finish_on_bram(void *p) {
    bram_args_t *a = p;
    a->ret = __real_mbedtls_sha256_finish(a->ctx, a->out);
}

int __wrap_mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char *output) {
    if (sp_on_bram_stack()) {
        return __real_mbedtls_sha256_finish(ctx, output);
    }
    bram_args_t a = { ctx, 0, NULL, NULL, NULL, NULL, output, 0 };
    on_bram_stack(sha256_finish_on_bram, &a);
    return a.ret;
}

/* ---- Ed25519 ------------------------------------------------------------- */

/* the group order L = 2^252 + 27742317777372353535851937790883648493 */
static const int64_t L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10
};

static void modL(u8 *r, int64_t x[64]) {
    int64_t carry;
    int i, j;
    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; ++j) {
        x[j] += carry - (x[31] >> 4) * L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; ++j) {
        x[j] -= carry * L[j];
    }
    for (i = 0; i < 32; ++i) {
        x[i + 1] += x[i] >> 8;
        r[i] = (u8)(x[i] & 255);
    }
}

/* r (64 bytes, a SHA-512 digest) -> r mod L in r[0..31] */
static void reduce(u8 *r) {
    int64_t x[64];
    for (int i = 0; i < 64; ++i) {
        x[i] = r[i];
    }
    memset(r, 0, 64);
    modL(r, x);
}

/* SHA-512 over up to three pieces */
static void sha512_3(u8 out[64], const void *a, size_t al, const void *b, size_t bl, const void *c, size_t cl) {
    mbedtls_sha512_context ctx;
    mbedtls_sha512_init(&ctx);
    mbedtls_sha512_starts(&ctx, 0);
    mbedtls_sha512_update(&ctx, a, al);
    if (bl) {
        mbedtls_sha512_update(&ctx, b, bl);
    }
    if (cl) {
        mbedtls_sha512_update(&ctx, c, cl);
    }
    mbedtls_sha512_finish(&ctx, out);
    mbedtls_sha512_free(&ctx);
}

/* seed -> clamped scalar a in d[0..31], prefix in d[32..63] */
static void expand_seed(u8 d[64], const u8 *seed) {
    sha512_3(d, seed, 32, NULL, 0, NULL, 0);
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;
}

/* out = encode(s * B), on the BRAM stack */
typedef struct {
    u8 out[32];
    const u8 *s;
} base_job;

static void base_run(void *arg) {
    base_job *job = arg;
    ge p;
    ge_scalarbase(&p, job->s);
    ge_tobytes(job->out, &p);
}

static void scalarbase_bytes(u8 out[32], const u8 *s) {
    base_job job = { .s = s };
    on_bram_stack(base_run, &job);
    memcpy(out, job.out, 32);
    memset(&job, 0, sizeof(job));
}

static void ed25519_public(u8 *pk, const u8 *seed) {
    u8 d[64];
    expand_seed(d, seed);
    scalarbase_bytes(pk, d);
    memset(d, 0, sizeof(d));
}

static void ed25519_sign(u8 *sig, const u8 *seed, const u8 *pk, const u8 *m, size_t n) {
    u8 d[64], r[64], h[64];
    int64_t x[64];
    expand_seed(d, seed);
    sha512_3(r, d + 32, 32, m, n, NULL, 0);
    reduce(r);
    scalarbase_bytes(sig, r);
    sha512_3(h, sig, 32, pk, 32, m, n);
    reduce(h);
    for (int i = 0; i < 64; ++i) {
        x[i] = 0;
    }
    for (int i = 0; i < 32; ++i) {
        x[i] = r[i];
    }
    for (int i = 0; i < 32; ++i) {
        for (int j = 0; j < 32; ++j) {
            x[i + j] += (int64_t)h[i] * d[j];
        }
    }
    modL(sig + 32, x);
    memset(d, 0, sizeof(d));
    memset(r, 0, sizeof(r));
    memset(x, 0, sizeof(x));
}

/* S must be canonical (S < L, RFC 8032 5.1.7), else signatures are malleable */
static int scalar_lt_L(const u8 *s) {
    for (int i = 31; i >= 0; --i) {
        if (s[i] < L[i]) {
            return 1;
        }
        if (s[i] > L[i]) {
            return 0;
        }
    }
    return 0;
}

/* encode(h * (-A) + S * B), on the BRAM stack; ok = 0 if A does not decode */
typedef struct {
    u8 out[32];
    const u8 *pk, *h, *s;
    int ok;
} verify_job;

static void verify_run(void *arg) {
    verify_job *job = arg;
    ge p, q;
    job->ok = 0;
    if (ge_frombytes_neg(&q, job->pk)) {
        return;
    }
    ge_scalarmult(&p, &q, job->h);
    ge_scalarbase(&q, job->s);
    ge_add(&p, &q);
    ge_tobytes(job->out, &p);
    job->ok = 1;
}

static int ed25519_verify(const u8 *pk, const u8 *m, size_t n, const u8 *sig) {
    u8 h[64];
    if (!scalar_lt_L(sig + 32)) {
        return 0;
    }
    sha512_3(h, sig, 32, pk, 32, m, n);
    reduce(h);
    verify_job job = { .pk = pk, .h = h, .s = sig + 32 };
    on_bram_stack(verify_run, &job);
    return job.ok && memcmp(job.out, sig, 32) == 0;
}

/* ---- Python bindings ----------------------------------------------------- */

static const u8 *get32(mp_obj_t o, const char *what) {
    mp_buffer_info_t bi;
    mp_get_buffer_raise(o, &bi, MP_BUFFER_READ);
    if (bi.len != 32) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("%s must be 32 bytes"), what);
    }
    return bi.buf;
}

static mp_obj_t c25519_x25519(size_t n_args, const mp_obj_t *args) {
    static const u8 nine[32] = {9};
    x25519_job job;
    memcpy(job.scalar, get32(args[0], "scalar"), 32);
    memcpy(job.point, n_args > 1 ? get32(args[1], "point") : nine, 32);
    on_bram_stack(x25519_run, &job);
    memset(job.scalar, 0, sizeof(job.scalar));
    u8 z = 0;
    for (int i = 0; i < 32; ++i) {
        z |= job.out[i];
    }
    if (z == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("low-order point"));
    }
    return mp_obj_new_bytes(job.out, 32);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(c25519_x25519_obj, 1, 2, c25519_x25519);

static mp_obj_t c25519_ed25519_public(mp_obj_t seed_in) {
    u8 seed[32], pk[32];
    memcpy(seed, get32(seed_in, "seed"), 32);
    ed25519_public(pk, seed);
    memset(seed, 0, sizeof(seed));
    return mp_obj_new_bytes(pk, 32);
}
static MP_DEFINE_CONST_FUN_OBJ_1(c25519_ed25519_public_obj, c25519_ed25519_public);

static mp_obj_t c25519_ed25519_sign(size_t n_args, const mp_obj_t *args) {
    u8 seed[32], pk[32], sig[64];
    memcpy(seed, get32(args[0], "seed"), 32);
    mp_buffer_info_t m;
    mp_get_buffer_raise(args[1], &m, MP_BUFFER_READ);
    if (n_args > 2) {
        memcpy(pk, get32(args[2], "public key"), 32);
    } else {
        ed25519_public(pk, seed);
    }
    ed25519_sign(sig, seed, pk, m.buf, m.len);
    memset(seed, 0, sizeof(seed));
    return mp_obj_new_bytes(sig, 64);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(c25519_ed25519_sign_obj, 2, 3, c25519_ed25519_sign);

static mp_obj_t c25519_ed25519_verify(mp_obj_t pk_in, mp_obj_t msg_in, mp_obj_t sig_in) {
    u8 pk[32];
    memcpy(pk, get32(pk_in, "public key"), 32);
    mp_buffer_info_t m, s;
    mp_get_buffer_raise(msg_in, &m, MP_BUFFER_READ);
    mp_get_buffer_raise(sig_in, &s, MP_BUFFER_READ);
    if (s.len != 64) {
        return mp_const_false;
    }
    return mp_obj_new_bool(ed25519_verify(pk, m.buf, m.len, s.buf));
}
static MP_DEFINE_CONST_FUN_OBJ_3(c25519_ed25519_verify_obj, c25519_ed25519_verify);

static const mp_rom_map_elem_t c25519_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_curve25519) },
    { MP_ROM_QSTR(MP_QSTR_x25519), MP_ROM_PTR(&c25519_x25519_obj) },
    { MP_ROM_QSTR(MP_QSTR_ed25519_public), MP_ROM_PTR(&c25519_ed25519_public_obj) },
    { MP_ROM_QSTR(MP_QSTR_ed25519_sign), MP_ROM_PTR(&c25519_ed25519_sign_obj) },
    { MP_ROM_QSTR(MP_QSTR_ed25519_verify), MP_ROM_PTR(&c25519_ed25519_verify_obj) },
};
static MP_DEFINE_CONST_DICT(c25519_globals, c25519_globals_table);

const mp_obj_module_t mp_module_curve25519 = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&c25519_globals,
};

MP_REGISTER_MODULE(MP_QSTR_curve25519, mp_module_curve25519);

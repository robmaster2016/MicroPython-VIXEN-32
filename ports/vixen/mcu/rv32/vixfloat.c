/* SPDX-License-Identifier: MIT
 * Copyright (c) 2025-2026 Robert S. (aka Robmaster) */
/* Single-precision float on the VIXEN-32 float co-processor (core v0.57+).
 *
 * The CPU has no FPU (rv32im, soft-float ABI): gcc calls libgcc's
 * __addsf3 & co. for every float operation. These definitions take their
 * place (the linker uses ours, libgcc's members are not pulled in): write
 * operand A, write operand B to GO(op) -- that starts the op -- read the
 * result; the read stalls on the bus until it is ready (divide ~35 clocks),
 * no polling. MicroPython's floats are single precision
 * (MICROPY_FLOAT_IMPL_FLOAT), so this is every Python float operation, and
 * the float maths of libm, the float formatting and parsing.
 *
 * Numbers (rtl/vih_fpu.sv): IEEE-754 binary32, round to nearest even, as
 * libgcc -- except that denormals (|x| < 1.18e-38) are flushed to zero in
 * and out, and a NaN comes back as the canonical quiet NaN. Float -> int
 * truncates and clamps. Comparisons stay libgcc's (cheap integer code).
 *
 * NOT reentrant: an interrupt handler between the A write and the GO write
 * would corrupt the operand. The port's interrupt handlers are empty
 * (interrupts.c) and use no floats.
 *
 * The operands and results are passed as their bit patterns (the soft-float
 * ABI passes floats in integer registers), so nothing here is a float
 * operation itself.
 */

#include <stdint.h>

#define FPU_A       (*(volatile uint32_t *)0x3A000200u)
#define FPU_RES     (*(volatile const uint32_t *)0x3A000208u)
#define FPU_GO(op)  (*(volatile uint32_t *)(0x3A000210u + (uint32_t)(op) * 4u))

enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_F2I, OP_I2F, OP_U2F, OP_F2U };

static inline uint32_t fpu(uint32_t op, uint32_t a, uint32_t b) {
    FPU_A = a;
    FPU_GO(op) = b;
    return FPU_RES;
}

uint32_t __addsf3(uint32_t a, uint32_t b) {
    return fpu(OP_ADD, a, b);
}

uint32_t __subsf3(uint32_t a, uint32_t b) {
    return fpu(OP_SUB, a, b);
}

uint32_t __mulsf3(uint32_t a, uint32_t b) {
    return fpu(OP_MUL, a, b);
}

uint32_t __divsf3(uint32_t a, uint32_t b) {
    return fpu(OP_DIV, a, b);
}

/* conversions: the operand goes in A, the GO write only starts the op */
int32_t __fixsfsi(uint32_t a) {
    return (int32_t)fpu(OP_F2I, a, 0u);
}

uint32_t __fixunssfsi(uint32_t a) {
    return fpu(OP_F2U, a, 0u);
}

uint32_t __floatsisf(int32_t a) {
    return fpu(OP_I2F, (uint32_t)a, 0u);
}

uint32_t __floatunsisf(uint32_t a) {
    return fpu(OP_U2F, a, 0u);
}

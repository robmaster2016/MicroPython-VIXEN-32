/* SPDX-License-Identifier: MIT */
/* VIXEN-32 memory map (DOOM-style: code in BRAM, heap in chip-RAM).
 *
 * Code (text/rodata/data, incl. gp-relative small data) lives in the on-chip
 * BRAM app region 0x100..0x6A000 -- exactly like every other VIXEN-32 .rvk app
 * (tut13, DOOM). VIXEN-32's resident .rvk loader (F12/Help -> Load RVK) places
 * the image there and enters it at _start == 0x100. VIXEN-32 itself is left
 * untouched (no BOOT.BIN, no trampoline).
 *
 * The interpreter's working memory -- bss, libc heap, C stack and the
 * MicroPython gc arena -- lives in the 16 MB cached chip-RAM window at
 * 0x40000000 (SDRAM), which the CPU reads/writes for data. This keeps the
 * ~423 KB BRAM budget for code only; the large heap lives in chip-RAM.
 *
 * Layout:
 *   BRAM      0x00000100  .text/.rodata/.data/.sdata/.sbss   code + globals
 *   chip-RAM  0x40000000  .bss (NOLOAD)                      zeroed at startup
 *   chip-RAM  0x00800000  .Heap  (NOLOAD)                    libc _sbrk area
 *   chip-RAM  0x00800000  .Stack (NOLOAD)                    C call stack, grows down
 *   chip-RAM  0x00900000  .gcram (NOLOAD)                    gc arena (MICROPY_HEAP_SIZE)
 *
 * The gc arena, heap and stack are NOLOAD: stored nowhere in the image;
 * entrypoint.s zeroes bss and fills the stack canary, main.c zeroes the gc
 * arena. The .rvk therefore carries only the BRAM code block (like apptest.rvk).
 *
 * ld quirks (verified with xPack riscv ld 13.2, see the previous chip-RAM
 * script): do NOT put an ALIGN()/address on a section *header* (it becomes the
 * section ADDRESS over the location counter, landing at 0); use ALIGN() inside.
 * Compute chip-RAM bases INLINE as ORIGIN(CHIP_RAM) + off (absolute), never as
 * `. = SYM` (ld treats that as a relative LC advance).
 */

OUTPUT_FORMAT("elf32-littleriscv", "elf32-littleriscv", "elf32-littleriscv")

ENTRY(_start)

_pointer_size = 4;

MEMORY
{
    /* On-chip BRAM app region (0x100 .. 0x6A000). Holds all code + data. */
    BRAM     (rwx) : ORIGIN = 0x00000100, LENGTH = 0x00069F00
    /* 16 MB cached chip-RAM window (SDRAM). Holds bss/heap/stack/gc only. */
    CHIP_RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 0x01000000
}

SECTIONS
{
    /* All loaded code + initialized data live in BRAM. GCC emits per-symbol
     * input sections (.text.foo, .rodata.bar.str1.4, .data.baz, .sdata.qux,
     * ...), so EVERY pattern below MUST use a trailing '*' -- a bare *(.text)
     * matches only a section named exactly ".text" and silently drops all the
     * .text.* ones as orphans. */
    .text : {
        *(.text.start)
        *(.start)
        *(.text*)
        *(.rodata*)
        *(.data*)
        /* gp-relative small data stays in BRAM near _global_pointer (+/-2 KB). */
        *(.sdata*)
        *(.srodata*)
        . = ALIGN(4);
        _global_pointer = .;
    } > BRAM

    /* Small (gp-relative) zero-init: BRAM, right after _global_pointer so the
     * gp-relative accesses stay in range. Zeroed by entrypoint.s (_sbss_bram..
     * _ebss_bram). */
    .sbss (NOLOAD) : {
        . = ALIGN(4);
        _sbss_bram = .;
        *(.sbss*)
        *(.scommon)
        _ebss_bram = .;
    } > BRAM

    /* Large bss (regular globals) in chip-RAM. Zeroed by entrypoint.s via the
     * chip-RAM window (NOLOAD: reserved, stored nowhere in the image). */
    .bss (NOLOAD) : {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*)
        *(COMMON)
        _ebss = .;
    } > CHIP_RAM

    /* libc heap for _sbrk: from the end of chip-RAM BSS up to the stack base. */
    .Heap (NOLOAD) : {
        _sheap = .;
        . = ORIGIN(CHIP_RAM) + 0x00800000 - MICROPY_STACK_SIZE;
        _eheap = .;
    } > CHIP_RAM

    /* C call stack: grows DOWN from 0x40800000 (MICROPY_STACK_SIZE tall). */
    .Stack (NOLOAD) : {
        _estack = ORIGIN(CHIP_RAM) + 0x00800000;
        _sstack = _estack - MICROPY_STACK_SIZE;
    } > CHIP_RAM

    /* MicroPython gc arena in chip-RAM. main.c zeroes this and hands it to
     * gc_init. Base 0x40900000; sized by MICROPY_HEAP_SIZE (via --defsym).
     * Advance the LC to the base first so the section STARTS there (a bare
     * `SYM = expr` only defines the symbol, it does not move the LC). */
    .gcram (NOLOAD) : {
        . = ORIGIN(CHIP_RAM) + 0x00900000;
        _GC_HEAP_START = .;
        . = _GC_HEAP_START + MICROPY_HEAP_SIZE;
        _GC_HEAP_END = .;
    } > CHIP_RAM
}

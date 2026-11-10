/* SPDX-License-Identifier: MIT
 * Copyright (c) 2025-2026 Robert S. (aka Robmaster) */
/* VIXEN-32 memory map for the LVGL build (make LVGL=1).
 *
 * MicroPython + LVGL + lv_binding_micropython are ~1.1 MB of code and
 * constants -- far more than the ~423 KB BRAM app region. The CPU can execute
 * from the cached chip-RAM window, but measured on the hardware that is 1.75x
 * slower for code that fits the 32 KB cache and ~5.4x slower beyond it. So:
 *
 *   BRAM   the hot paths: LVGL's software renderer, refresh, style/area/colour
 *          helpers, font glyph lookup, the MicroPython VM and object core, the
 *          port itself, libc/libgcc; all small (gp-relative) data; the LVGL
 *          render buffer (.bram_bss).
 *   chip   everything else, code and constants: the generated binding
 *          (lv_mpy.c), the compiler/parser, widgets, themes, layouts, font
 *          bitmaps, lodepng, extmod, libm. Runs/reads through the cache.
 *
 * Chip-RAM layout (byte offsets in the 16 MB window at 0x40000000):
 *   0x000000  .bss, then the libc heap (_sbrk) up to 0x0E0000
 *   0x0E0000  reserved: sprite image slots 0xE0..0xFF (mouse cursor = 0xF0)
 *   0x100000  .chiptext: code + rodata + data, loaded as an sdram block
 *   0x300000  reserved: three 128 KB framebuffers (vx_lvgl.c)
 *   0x380000  the sound module's window (vixen_sound.c) up to 0x7F0000
 *   (the C stack is in BRAM, after .bram_bss: see .Stack below)
 *   0x800000  reserved: the .rvk loader's staging area -- must stay free
 *   0x900000  MicroPython gc arena (MICROPY_HEAP_SIZE), LVGL allocates here too
 *   0xF10000  kernel modules (kmodnet) -- not ours
 *
 * Same ld quirks as vixen.ld (see there): no address on section headers,
 * bases computed inline as ORIGIN(CHIP_RAM) + off. Patterns end in '*' so the
 * per-function input sections (.text.foo) match.
 */

OUTPUT_FORMAT("elf32-littleriscv", "elf32-littleriscv", "elf32-littleriscv")

ENTRY(_start)

_pointer_size = 4;

MEMORY
{
    BRAM     (rwx) : ORIGIN = 0x00000100, LENGTH = 0x00069F00
    CHIP_RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 0x01000000
}

SECTIONS
{
    .text : {
        *(.text.start)
        *(.start)

        /* the port */
        *mcu/rv32/*.o(.text* .rodata*)
        *mcu/riscv/*.o(.text* .rodata*)
        */main.o(.text* .rodata*)
        */uart.o(.text* .rodata*)
        */mphalport.o(.text* .rodata*)
        */syscalls.o(.text* .rodata*)
        */vx_lvgl.o(.text* .rodata*)
        */vx_lvgl_mp.o(.text* .rodata*)
        /* curve25519: field multiply and square, most of every X25519/Ed25519
         * operation (its stack is in .bram_bss, see vixen_25519.c) */
        */vixen_25519.o(.text.fe_mul .text.fe_sq)
        /* AES encryption (CTR for sshd, GCM for TLS) and SHA-256: the bulk
         * data path of every encrypted transfer (~11 KB). They run on the
         * same BRAM stack (__wrap_ functions in vixen_25519.c). */
        *lib/mbedtls/library/aes.o(.text.mbedtls_internal_aes_encrypt .text.mbedtls_aes_crypt_ecb .text.mbedtls_aes_crypt_ctr .rodata.FT0 .rodata.FT1 .rodata.FT2 .rodata.FT3 .rodata.FSb)
        *lib/mbedtls/library/sha256.o(.text* .rodata*)
        *lib/mbedtls/library/gcm.o(.text.gcm_mult .text.gcm_mask .text.mbedtls_gcm_update .rodata.last4)
        *shared/runtime/*.o(.text* .rodata*)
        *shared/libc/*.o(.text* .rodata*)

        /* MicroPython: VM and object core (code; the qstr pool stays in chip) */
        *py/vm.o(.text* .rodata*)
        *py/runtime.o(.text* .rodata*)
        *py/runtime_utils.o(.text* .rodata*)
        *py/nativeglue.o(.text* .rodata*)
        *py/bc.o(.text* .rodata*)
        *py/obj.o(.text* .rodata*)
        *py/objtype.o(.text* .rodata*)
        *py/objfun.o(.text* .rodata*)
        *py/objboundmeth.o(.text* .rodata*)
        *py/objclosure.o(.text* .rodata*)
        *py/objint.o(.text* .rodata*)
        *py/objint_mpz.o(.text*)
        *py/mpz.o(.text*)
        *py/objlist.o(.text* .rodata*)
        *py/objtuple.o(.text* .rodata*)
        *py/objdict.o(.text* .rodata*)
        *py/objstr.o(.text*)
        *py/objexcept.o(.text*)
        *py/objgenerator.o(.text* .rodata*)
        *py/objrange.o(.text* .rodata*)
        *py/objslice.o(.text* .rodata*)
        *py/objfloat.o(.text*)
        *py/map.o(.text* .rodata*)
        *py/gc.o(.text* .rodata*)
        *py/qstr.o(.text*)
        *py/mpstate.o(.text* .rodata*)
        *py/nlr*.o(.text* .rodata*)
        *py/stackctrl.o(.text* .rodata*)
        *py/cstack.o(.text* .rodata*)
        *py/scheduler.o(.text* .rodata*)
        *py/binary.o(.text*)
        *py/malloc.o(.text* .rodata*)
        *py/vstr.o(.text* .rodata*)
        *py/mpprint.o(.text*)

        /* LVGL: rendering and everything a refresh touches */
        *lvgl/src/draw/sw/*(.text* .rodata*)
        *lvgl/src/draw/lv_draw.o(.text* .rodata*)
        *lvgl/src/draw/lv_draw_buf.o(.text* .rodata*)
        *lvgl/src/draw/lv_draw_rect.o(.text* .rodata*)
        *lvgl/src/draw/lv_draw_label.o(.text* .rodata*)
        *lvgl/src/draw/lv_draw_image.o(.text* .rodata*)
        *lvgl/src/draw/lv_draw_arc.o(.text* .rodata*)
        *lvgl/src/draw/lv_draw_line.o(.text* .rodata*)
        *lvgl/src/draw/lv_draw_mask.o(.text* .rodata*)
        *lvgl/src/draw/lv_draw_triangle.o(.text* .rodata*)
        *lvgl/src/core/lv_refr.o(.text* .rodata*)
        *lvgl/src/core/lv_obj_draw.o(.text* .rodata*)
        *lvgl/src/core/lv_obj_style.o(.text* .rodata*)
        *lvgl/src/core/lv_obj_style_gen.o(.text* .rodata*)
        *lvgl/src/core/lv_obj_class.o(.text* .rodata*)
        *lvgl/src/core/lv_obj_tree.o(.text* .rodata*)
        *lvgl/src/core/lv_obj_event.o(.text* .rodata*)
        *lvgl/src/display/*(.text* .rodata*)
        *lvgl/src/misc/lv_area.o(.text* .rodata*)
        *lvgl/src/misc/lv_color.o(.text* .rodata*)
        *lvgl/src/misc/lv_color_op.o(.text* .rodata*)
        *lvgl/src/misc/lv_math.o(.text* .rodata*)
        *lvgl/src/misc/lv_style.o(.text* .rodata*)
        *lvgl/src/misc/lv_ll.o(.text* .rodata*)
        *lvgl/src/misc/lv_array.o(.text* .rodata*)
        *lvgl/src/misc/lv_timer.o(.text* .rodata*)
        *lvgl/src/misc/lv_anim.o(.text* .rodata*)
        *lvgl/src/misc/lv_event.o(.text* .rodata*)
        *lvgl/src/misc/lv_text.o(.text* .rodata*)
        *lvgl/src/misc/lv_utils.o(.text* .rodata*)
        *lvgl/src/font/lv_font.o(.text* .rodata*)
        *lvgl/src/font/fmt_txt/*(.text*)
        *lvgl/src/stdlib/*(.text* .rodata*)
        *lvgl/src/tick/*(.text* .rodata*)

        /* C runtime */
        *libc.a:*(.text* .rodata*)
        *libgcc.a:*(.text* .rodata*)

        /* gp-relative small data stays in BRAM near _global_pointer (+/-2 KB). */
        *(.sdata*)
        *(.srodata*)
        . = ALIGN(4);
        _global_pointer = .;
    } > BRAM

    .sbss (NOLOAD) : {
        . = ALIGN(4);
        _sbss_bram = .;
        *(.sbss*)
        *(.scommon)
        _ebss_bram = .;
    } > BRAM

    /* Uninitialised BRAM buffers the CPU writes per pixel (LVGL's render
     * buffer): tagged __attribute__((section(".bram_bss"))). Not zeroed. */
    .bram_bss (NOLOAD) : {
        . = ALIGN(8);
        *(.bram_bss*)
        _ebram = .;
    } > BRAM

    .bss (NOLOAD) : {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*)
        *(COMMON)
        _ebss = .;
    } > CHIP_RAM

    .Heap (NOLOAD) : {
        _sheap = .;
        . = ORIGIN(CHIP_RAM) + 0x000E0000;
        _eheap = .;
    } > CHIP_RAM

    .Sprites (NOLOAD) : {
        . = ORIGIN(CHIP_RAM) + 0x00100000;
    } > CHIP_RAM

    /* Everything not claimed above: code, constants and initialised data,
     * executed/read through the cache. Loaded by the .rvk as an sdram block at
     * chip address 0x100000; entrypoint.s invalidates the cache first. */
    .chiptext : {
        _schiptext = .;
        *(.text*)
        *(.rodata*)
        *(.data*)
        . = ALIGN(4);
        _echiptext = .;
    } > CHIP_RAM

    .Framebuffers (NOLOAD) : {
        . = ORIGIN(CHIP_RAM) + 0x00300000;
        . = ORIGIN(CHIP_RAM) + 0x00360000;
    } > CHIP_RAM

    /* The C stack in BRAM. MicroPython keeps every Python call's frame on
     * it, and in chip RAM each of those stores is a write-through to the
     * SDRAM (~40 cycles): measured in vxemu, half of all chip-RAM stores of
     * an sshd transfer went to the stack. LVGL's deepest path used ~5.7 KB,
     * a TLS handshake ~4 KB. */
    .Stack (NOLOAD) : {
        . = ALIGN(16);
        _sstack = .;
        . = . + MICROPY_STACK_SIZE;
        _estack = .;
    } > BRAM

    .gcram (NOLOAD) : {
        . = ORIGIN(CHIP_RAM) + 0x00900000;
        _GC_HEAP_START = .;
        . = _GC_HEAP_START + MICROPY_HEAP_SIZE;
        _GC_HEAP_END = .;
    } > CHIP_RAM

    ASSERT(_ebss <= ORIGIN(CHIP_RAM) + 0x000E0000, "chip-RAM .bss runs into the sprite slots")
    ASSERT(_echiptext <= ORIGIN(CHIP_RAM) + 0x00300000, ".chiptext runs into the framebuffers")
    ASSERT(_estack <= ORIGIN(BRAM) + LENGTH(BRAM), "C stack does not fit into BRAM")
    ASSERT(_GC_HEAP_END <= ORIGIN(CHIP_RAM) + 0x00F00000, "gc arena runs into the kernel modules")
}

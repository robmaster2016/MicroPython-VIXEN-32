/* SPDX-License-Identifier: MIT
 * Copyright (c) 2025-2026 Robert S. (aka Robmaster) */
/* VIXEN-32 memory map for the network build (make NET=1, LVGL=0).
 *
 * The plain interpreter (~350 KB) fits the ~423 KB BRAM app region on its
 * own; lwIP + mbedtls + the frozen mip/requests add ~250 KB more. Rather than
 * the LVGL build's opt-in list of hot objects, this keeps EVERYTHING in BRAM
 * as before -- the interpreter runs exactly as fast as without the network --
 * and moves only the network libraries out to the cached chip-RAM window
 * (1.75x slower in cache, ~5.4x beyond it, measured). The per-packet paths of
 * lwIP and the symmetric crypto of a TLS session are pulled back into BRAM;
 * the handshake-only code (X.509, RSA, ECP, the TLS state machine) stays in
 * chip RAM, it runs once per connection.
 *
 * Chip-RAM layout is the LVGL build's (vixen_lvgl.ld), framebuffers unused:
 *   0x000000  .bss, then the libc heap (_sbrk) up to 0x0E0000
 *   0x0E0000  reserved: sprite image slots
 *   0x100000  .chiptext: loaded as an sdram block by the .rvk
 *   0x380000  the sound module's window (vixen_sound.c) up to 0x7F0000
 *   0x7F8000  C stack, grows down from 0x800000 (MICROPY_STACK_SIZE)
 *   0x800000  reserved: the .rvk loader's staging area -- must stay free
 *   0x900000  MicroPython gc arena (MICROPY_HEAP_SIZE)
 *   0xF10000  kernel modules (kmodnet) -- not ours
 *
 * Same ld quirks as vixen.ld (see there).
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

        /* network hot paths: every frame, every TLS record */
        */vixen_eth.o(.text* .rodata*)
        *lib/lwip/src/core/tcp_in.o(.text* .rodata*)
        *lib/lwip/src/core/tcp_out.o(.text* .rodata*)
        *lib/lwip/src/core/tcp.o(.text* .rodata*)
        *lib/lwip/src/core/pbuf.o(.text* .rodata*)
        *lib/lwip/src/core/inet_chksum.o(.text* .rodata*)
        *lib/lwip/src/core/ipv4/ip4.o(.text* .rodata*)
        *lib/lwip/src/core/ipv4/ip4_addr.o(.text* .rodata*)
        *lib/lwip/src/core/ipv4/etharp.o(.text* .rodata*)
        *lib/lwip/src/netif/ethernet.o(.text* .rodata*)
        *lib/lwip/src/core/mem.o(.text* .rodata*)
        *lib/lwip/src/core/memp.o(.text* .rodata*)
        *lib/lwip/src/core/timeouts.o(.text* .rodata*)
        *lib/lwip/src/core/def.o(.text* .rodata*)
        *extmod/modlwip.o(.text* .rodata*)
        *lib/mbedtls/library/aes.o(.text* .rodata*)
        *lib/mbedtls/library/gcm.o(.text* .rodata*)
        *lib/mbedtls/library/sha256.o(.text* .rodata*)
        *lib/mbedtls/library/bignum_core.o(.text* .rodata*)
        /* curve25519: field multiply and square, most of every X25519/Ed25519
         * operation (its stack is in .bram_bss, see vixen_25519.c) */
        */vixen_25519.o(.text.fe_mul)

        /* everything else except the rest of the network code: libraries,
         * TLS/NIC bindings, the frozen modules and the sound module */
        *(EXCLUDE_FILE(*lib/lwip/* *lib/mbedtls/* *lib/mbedtls_errors/* *frozen_content.o *extmod/modtls_mbedtls.o *extmod/mbedtls/* *extmod/network_lwip.o *extmod/modnetwork.o *extmod/modwebsocket.o *extmod/modwebrepl.o */vixen_sound.o */vixen_random.o */vixen_25519.o *extmod/os_dupterm.o *extmod/modcryptolib.o) .text*)
        *(EXCLUDE_FILE(*lib/lwip/* *lib/mbedtls/* *lib/mbedtls_errors/* *frozen_content.o *extmod/modtls_mbedtls.o *extmod/mbedtls/* *extmod/network_lwip.o *extmod/modnetwork.o *extmod/modwebsocket.o *extmod/modwebrepl.o */vixen_sound.o */vixen_random.o */vixen_25519.o *extmod/os_dupterm.o *extmod/modcryptolib.o) .rodata*)
        *(EXCLUDE_FILE(*lib/lwip/* *lib/mbedtls/* *lib/mbedtls_errors/* *frozen_content.o *extmod/modtls_mbedtls.o *extmod/mbedtls/* *extmod/network_lwip.o *extmod/modnetwork.o *extmod/modwebsocket.o *extmod/modwebrepl.o */vixen_sound.o */vixen_random.o */vixen_25519.o *extmod/os_dupterm.o *extmod/modcryptolib.o) .data*)

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

    /* Uninitialised BRAM buffers, tagged __attribute__((section(".bram_bss"))):
     * the curve25519 stack. Not zeroed. */
    .bram_bss (NOLOAD) : {
        . = ALIGN(16);
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

    /* The network libraries: code, constants and initialised data, executed/
     * read through the cache. Loaded by the .rvk as an sdram block at chip
     * address 0x100000; entrypoint.s invalidates the cache first. */
    .chiptext : {
        _schiptext = .;
        *(.text*)
        *(.rodata*)
        *(.data*)
        . = ALIGN(4);
        _echiptext = .;
    } > CHIP_RAM

    .Stack (NOLOAD) : {
        . = ORIGIN(CHIP_RAM) + 0x00800000 - MICROPY_STACK_SIZE;
        _sstack = .;
        . = ORIGIN(CHIP_RAM) + 0x00800000;
        _estack = .;
    } > CHIP_RAM

    .gcram (NOLOAD) : {
        . = ORIGIN(CHIP_RAM) + 0x00900000;
        _GC_HEAP_START = .;
        . = _GC_HEAP_START + MICROPY_HEAP_SIZE;
        _GC_HEAP_END = .;
    } > CHIP_RAM

    ASSERT(_ebss <= ORIGIN(CHIP_RAM) + 0x000E0000, "chip-RAM .bss runs into the sprite slots")
    ASSERT(_echiptext <= ORIGIN(CHIP_RAM) + 0x00380000, ".chiptext runs into the sound window")
    ASSERT(_sstack >= ORIGIN(CHIP_RAM) + 0x007F0000, "C stack runs into the sound window")
    ASSERT(_GC_HEAP_END <= ORIGIN(CHIP_RAM) + 0x00F00000, "gc arena runs into the kernel modules")
}

# Architecture of the VIXEN-32 port

How the port is put together: memory layout, console, SD card, LVGL, network,
sound and timebase. For building and installing see [building.md](building.md).

VIXEN-32 is a 32-bit RISC-V home computer (VexRiscv "Murax" core, rv32im_zicsr /
ilp32) that runs as a core on the MEGA65 (AMD Artix-7 200T FPGA). This port builds
a single `firmware.elf` / `firmware.bin` that runs the full MicroPython feature set
on it.

## Memory model: DOOM-style (code in BRAM, heap in chip-RAM)

VIXEN-32 has two addressable memories that matter here:

* 512 KB zero-wait on-chip BRAM (`0x000000`..`0x7FFFFF`). The resident bootloader +
  KERNAL live above `0x6A000`; a standard `.rvk` app only owns ~423 KB (`0x100`..
  `0x6A000`).
* a 16 MB **cached chip-RAM window** at `0x40000000` (`VIH_SDRAM_CACHED_BASE`,
  SDRAM). Since core v0.34 it is readable/writable; since core v0.39 the CPU can
  also **fetch instructions** from it.

This port follows the same layout as every other VIXEN-32 `.rvk` app (tut13,
DOOM): the *code* -- text/rodata/data, incl. gp-relative small data -- lives in
BRAM (`mcu/rv32/vixen.ld`), and the interpreter's *working memory* -- bss, the
libc heap, the C call stack and the MicroPython gc arena -- lives in the chip-RAM
window. That keeps the ~423 KB BRAM budget for code only (the full feature set
fits: loaded code is ~345 KB) while the large heap lives in chip-RAM.

Layout (`mcu/rv32/vixen.ld`):

    0x00000100  .text/.rodata/.data/.sdata/.sbss   code + globals  (~340 KB, BRAM)
    0x40000000  .bss / .Heap                       zeroed at startup, libc _sbrk
    0x40800000  .Stack                             C call stack, grows DOWN (8 KB)
    0x40900000  .gcram                             MicroPython gc arena (1 MB)

The image is entered at `_start == 0x100` (BRAM). `.bss`/`.Heap`/`.Stack`/`.gcram`
are NOLOAD: they are reserved in chip-RAM but stored nowhere in the image --
`entrypoint.s` zeroes bss and fills the stack canary, `main.c` zeroes the gc arena
before `gc_init`. Because the heap carries no bytes, the `.rvk` is a single BRAM
code block, exactly like `apptest.rvk`.

## ld quirks worked around in vixen.ld

Two non-obvious behaviours of the xPack riscv `ld` (13.2) shaped the script:

1. A section header address expression is NOT an alignment. `.text ALIGN(4096) :`
   reads `ALIGN(4096)` as the section's *address* (over the location counter, which
   starts at 0) and lands outside the region. The header address is omitted so ld
   drops the section at the region origin; page boundaries use `. = ALIGN(4096)`
   *inside* the section.
2. A bare `SYM = 0x40900000;` used as `. = SYM` is treated as a *relative* advance
   (`LC += SYM`), overflowing the region -- even when `SYM` is defined from
   `ORIGIN()`. The fixed bases (gc arena, stack top) are computed inline as
   `ORIGIN(CHIP_RAM) + off` (a true absolute expression), and each NOLOAD region
   advances the LC to its base first so the section starts there.

`entrypoint.s` lives in a section named `.start`; the script globs both
`*(.text.start)` and `*(.start)` so `_start` lands first, at `0x100`. The small
data (`.sdata`/`.sbss`) is kept in BRAM next to `_global_pointer` because it is
addressed gp-relative (within +/-2 KB); the large `.bss` moves to chip-RAM with
absolute addressing.

## Console: VIH text layer + MEGA65 keyboard

VIXEN-32 has no UART; `uart.c` implements the MicroPython console on the VIH
hardware directly.

**Screen.** A 48x28 software screen -- the whole visible tile grid, 384x224 px,
no border -- is mirrored into tile map 0 (built-in 8x8 ROM font, one 32-bit cell
per character). Earlier versions used 40x25 inside a border because the top
tile row always looked cut by ~4 lines; the cause was the tile scroll that
`vih_init_registers()` sets (x=1, y=4 pixels, moving the text layer up and left).
`uart_init()` sets it to 0,0 (`TILE_SCROLL_X/Y`); `vixen.tile_scroll(x, y)`
changes it at run time, e.g. to find an offset should the hardware need one
(the GFX layer, by comparison, needs a fine X scroll of 4 without a border).
Writing `border_dims`/`tile_scroll` claims sticky ownership of the registers --
fine, the port runs no copper.

The cursor is a blinking reverse-video block (300 ms phase). Output understands the
VT100 subset MicroPython's readline emits: `\b` moves left without erasing,
`ESC [ n D/C/A/B` move the cursor (D/C wrap across rows, because output wraps
immediately after column 39), `ESC [ K` erases from the cursor to the end of the
screen (a readline line may span several rows), `ESC [ J`, `ESC [ 2 J` and
`ESC [ H` clear/home. Other escape sequences are swallowed instead of printed.

**Keyboard.** On the MEGA65 the `0x190` FIFO carries native matrix key numbers
(`code = 0x80 | key`, C65 matrix order); PS/2 set-2 codes (< 0x80) are still
decoded for MiSTer builds. `uart_poll()` drains the 8-deep hardware FIFO into a
64-byte software buffer and is called both while the REPL waits for input and
from the VM (`MICROPY_VM_HOOK_POLL`, every 64 bytecodes), so type-ahead is not
lost while code runs.

* INST/DEL is backspace, the cursor keys send `ESC [ A/B/C/D`.
* MEGA (C=) or ALT + `:` `;` `£` `-` `@` type `{` `}` `|` `~` `` ` `` -- characters
  the MEGA65 keyboard has no key for, which Python needs.
* Ctrl+letter gives ^A..^Z (an earlier version subtracted 'A' from the
  lowercase letter: ctrl+c gave `#`, ctrl+d `$`).
* Typematic repeat in software: a held key repeats after 500 ms, then every 50 ms
  (`KBD_RPT_DELAY_MS`/`KBD_RPT_RATE_MS`). Printable keys, DEL, TAB and the cursor
  keys repeat; RETURN, ESC and Ctrl combinations do not.
* Ctrl-C and RUN/STOP raise `KeyboardInterrupt` in running code, including inside
  `time.sleep()` and `input()`. At the REPL prompt Ctrl-C cancels the line as
  usual.

## Autostart

The port keeps its files on the SD card under one folder,
`/sd/vixen-32/apps/micropython` (`VIXEN_APP_DIR` in `main.c`):

    boot/boot.py   boot/main.py   run at start-up, in this order
    examples/      the example scripts (on sys.path)
    lib/           your own modules (on sys.path)

At power-up and after every soft reset (Ctrl-D) `main.c` changes into that folder
(it becomes the current directory) and runs `boot/boot.py` and then
`boot/main.py` if they exist (`pyexec_file_if_exists`), like other MicroPython
boards. RUN/STOP or Ctrl-C interrupts a running script and drops to the REPL.

## SD card and disk images

`vixen_disk.c` adds the module `vixen` with a VFS class `vixen.Disk(unit)`. It
does not parse FAT itself: every operation goes through the resident kernel's disk
services (devkit `common/kernel_api.h`), the same FatFs instance BASIC and DOS use.
That keeps the kernel the only writer of the filesystem -- a second FatFs on the
card (`VfsFat` over raw sectors) would work from stale caches and could corrupt
it. Long file names, sub-directories and the kernel's per-unit current directory
come for free.

At startup (and after a soft reset) `vixen_disk_automount()` mounts every unit
that answers:

    /sd   unit 3 (vixen.UNIT_SD)  the SD card itself
    /a    unit 0 (vixen.UNIT_A)   disk image in drive A: (F12 menu)
    /b    unit 1 (vixen.UNIT_B)   disk image in drive B:

Absent, unformatted or disabled units are skipped silently. When `/sd` is
mounted, `/sd/vixen-32/apps/micropython/lib` and `.../examples` are appended to
`sys.path`, so modules import from the card. Units can also be mounted by hand:

    import os, vixen
    os.mount(vixen.Disk(vixen.UNIT_A), '/a')
    os.mount(vixen.Disk(vixen.UNIT_SD), '/sd', readonly=True)

Supported: `open()` in `'r'`/`'w'` (text or binary), `read`/`readinto`/
`readline(s)`/`write`/`seek`/`tell`, `os.listdir`/`ilistdir`, `stat`, `mkdir`,
`remove`, `rmdir`, `rename`, `chdir`/`getcwd`, `statvfs`, `import`.

Limits that come from the kernel API:

* **Drive C: (unit 3) must be enabled in BOOT.BIN.** Since BOOT.BIN build 119 the
  SD card unit is switched off by default (`VX_DRIVE_C` in `risc/basic_test.c`)
  and the kernel answers "no such unit"; `/sd` then does not appear. A: and B:
  work regardless. With C: enabled, the kernel may restrict *writes* to
  `/VIXEN-32/` (the same jail SAVE uses).
* No append and no read+write handles: modes `'a'`, `'+'` and `'x'` raise
  `OSError(EOPNOTSUPP)`.
* No fsync: written data is committed when the file is closed.
* **SD card ownership.** Any kernel access to unit 3 claims the card from the
  MEGA65 side and keeps it until `kdisk_release()`. While it is held, pressing
  F12 stops the core with "SD Card: Unknown error while trying to browse. Error
  code: 3707". The driver therefore releases the card after every operation, as
  BASIC does -- except while one of its files is open on the card, because a
  release unmounts the filesystem the handle depends on. **Close files before
  pressing F12**; use `with open(...)` (the GC closes forgotten files only
  eventually). `ilistdir()` reads the whole directory and closes it at once.
* Because a release unmounts the kernel's filesystem, the kernel's own current
  directory is not used: each `Disk` keeps its cwd itself (`os.chdir`) and passes
  the kernel absolute, normalised paths only (`.` and `..` are resolved by the
  driver).
* Only the services the installed BOOT.BIN exports are used; a missing kernel
  entry raises `OSError(ENODEV)` instead of jumping to address 0.

## LVGL (make LVGL=1)

The LVGL build adds the `lvgl` module from
[lv_binding_micropython](https://github.com/lvgl/lv_binding_micropython)
(LVGL 9.3.0, the version the binding pins) plus a display/mouse driver for the
VIXEN hardware. Without `LVGL=1` none of it is built in.

    import lvgl as lv, vixen     # the import runs lv_init()
    vixen.lv_display()           # 384x224 display + mouse; console hidden
    b = lv.button(lv.screen_active())
    lv.label(b).set_text("Hello VIXEN")
    b.center()

* No main loop: `lv_timer_handler()` runs every 5 ms from the console poll
  (REPL wait, `time.sleep()`, every 64 bytecodes) through the MicroPython
  scheduler, i.e. only between bytecodes, never inside an LVGL call.
* **F1** switches between the GUI and the REPL console. The keyboard types into
  the REPL, except while the GUI is shown and a handler is registered with
  `vixen.lv_keys(handler)`: then every key goes to `handler(key)` as an LVGL key
  code (TAB = `lv.KEY.NEXT`, shift+TAB = `lv.KEY.PREV`, cursor keys =
  `lv.KEY.UP/DOWN/LEFT/RIGHT`, RETURN = `lv.KEY.ENTER`, others their ASCII
  code), run by the scheduler. Ctrl-C and F1 always stay with the console;
  `vixen.lv_keys(None)` removes the handler. LVGL gets the mouse (Amiga mouse in port 1, hardware
  sprite cursor). `vixen.lv_dither(True)` turns on 4x4 ordered dithering.
* Colours: the display driver loads its 256-colour palette over the C64 one.
  With the GUI up, background and border are index 0 (black) -- index 6, the
  console's blue, is a dark green in the new palette and showed as a green
  strip at the right edge. The console switches to the palette's nearest C64
  blues (44/88/215) from then on.
* Soft reset (Ctrl-D) works: the port resets the binding's "initialised" flag,
  so the next `import lvgl` starts LVGL afresh.
* PNG images (lodepng) with a 512 KB decoded-image cache:
  `d = open('/sd/x.png','rb').read(); img.set_src(lv.image_dsc_t({'data_size': len(d), 'data': d}))`.
* Examples (copy to `/sd/vixen-32/apps/micropython/examples/`, then e.g.
  `import lvgl_demo`; TAB / shift+TAB switch the tabs in the demo and gallery):
  `lvgl_demo.py` (the C demo in Python: tabs, widgets, live chart),
  `widgets_gallery.py` (every widget once + a pass/fail report -- 30 of 30 in
  the emulator), `png_demo.py` + `vixen.png` (load, show and drag a PNG),
  `mouse_test.py` (mouse register incl. raw pin levels; both builds).
* User guide with the widget table and a quick check:
  [lvgl.md](lvgl.md).
* LVGL 9.3 details: `LV_ANIM_OFF/ON` are plain bools (`False`/`True`), there
  is no `lv.init()`.

**Display driver** (`vx_lvgl.c`, `vx_lvgl_mp.c`; tuned on the MEGA65): RGB565 rendering into a 384x24 BRAM
buffer, conversion through a 64 KB table to a 256-colour palette (6x6x6 cube +
40 greys), triple buffering with blitter sync, areas rounded to 4-pixel
columns, GFX fine X scroll 4 (needed without a border).

**Memory** (`mcu/rv32/vixen_lvgl.ld`). MicroPython + LVGL + binding are
~1.15 MB; BRAM holds 423 KB. BRAM keeps the hot paths -- LVGL's software
renderer, refresh, style/area/colour helpers, font glyph lookup, the VM and
object core, the port, libc -- plus the render buffer and the colour table
(~392 KB in use). Everything else (binding, compiler, widgets, themes, font
bitmaps, lodepng, extmod) runs from chip RAM through the cache, loaded as an
sdram block of the .rvk. Chip RAM: .bss + libc heap 0x000000, sprite slots
0x0E0000, code 0x100000-0x300000, framebuffers 0x300000, loader staging
0x800000 (kept free), gc heap 4 MB at 0x900000. The C stack (16 KB) is in
BRAM, after .bram_bss: every Python call keeps its frame there, and in chip
RAM each store is a write-through to the SDRAM (half of all chip-RAM stores
of an sshd transfer, measured in vxemu). Peak use measured: LVGL ~5.7 KB, TLS
~4 KB, sshd + SD on the hardware 6.4 KB. Python recursion: ~48 levels
(MICROPY_STACK_CHECK_MARGIN 2 KB). AES (encrypt), SHA-256 and the GCM core
are in BRAM too and run on the curve25519 BRAM stack (`__wrap_` functions in
vixen_25519.c, `-Wl,--wrap` in the Makefile), in both network builds.

**Binding patches** (`patches/lv_binding-vixen.patch`, applied by `setup.sh`):

* `gen/gen_mpy.py`: MicroPython 1.29 renamed `mp_obj_int_to_bytes_impl()`.
* `micropython.mk`: preprocess `lvgl.h` too -- `lvgl_private.h` alone does not
  pull in widgets without a private header (no `lv.spinner`, `lv.list`, ...).
* `lvgl/src/stdlib/micropython/lv_mem_core_micropython.c`: allocate with no gc
  flags. `gc_alloc(size, true)` now means GC_ALLOC_FLAG_HAS_FINALISER: every
  LVGL block looked like a Python object with `__del__`, and the gc crashed
  reading raw LVGL memory as a type pointer (seen on soft reset).

Build: `./build.sh LVGL=1 BUILD=build-lvgl-net` (with network) or
`./build.sh LVGL=1 NET=0 BUILD=build-lvgl`.

## Network (make NET=1, the default)

`network.LAN`, `socket`, `ssl`, and the frozen `mip`, `requests` and `net`.
lwIP runs directly on the core's Ethernet MAC (`vixen_eth.c`, Chipset
Reference chapter 11); mbedtls provides TLS. The kernel's own network stack
(SHIFT+POUND in BASIC) is not used: it is a stop-and-wait TCP client with two
connections, and both stacks would compete for the one RX window.

    import net
    lan = net.up()          # NETWORK.TXT: "ip: dhcp" or ip/mask/gw/dns, mac
    import mip
    mip.install("base64")     # -> /sd/vixen-32/apps/micropython/lib

Or by hand: `lan = network.LAN(); lan.active(True)`, then wait for
`lan.isconnected()`. `lan.status("stats")` returns (frames received, frames
sent, hardware drops, CRC errors). `lan.config(rx_late=0/1)` pins the RMII
sampling edge (ETH_CTRL bit 3); by default it is found at run time: until the
first good frame arrives, every two CRC errors flip it.

How it runs, without interrupts:

* `vixen_eth_poll()` drains the RX window (at most the 4 frames the hardware
  holds) and runs lwIP's timers. It is called from `uart_poll()` -- the REPL's
  input wait, `time.sleep()`, the VM hook -- and from modlwip's blocking loops
  (`MICROPY_PY_LWIP_POLL_HOOK`). modlwip's ENTER/EXIT is a counter the poll
  respects; it is reset on soft reset.
* The packet windows take 32-bit accesses only: frames go through a RAM bounce
  buffer and are moved as words. The core does not pad, so short frames are
  padded to 60 bytes in software.
* `TCP_WND` is 4 x MSS on purpose: a peer can never have more segments in
  flight than the four RX slots hold, even while a TLS handshake keeps the CPU
  away from the poll. With 8 x MSS the emulator showed drops and resends.
* The interface survives a soft reset; open sockets are closed by their
  finalisers (`gc_sweep_all()` in `main.c`).
* No RTC: mbedtls is built without `MBEDTLS_HAVE_TIME`; certificate dates are
  not checked (MicroPython's `ssl` does not verify certificates by default).
* No hardware RNG: `vixen_random.c` runs a SHA-256 DRBG (network builds) fed by
  key and Ethernet frame timing, the raster beam, boot jitter and a seed file
  (`random.seed`) that is read and rewritten once per power-up. `os.urandom`,
  TLS and lwIP draw from it. See [network.md](network.md).
* Multicast does not pass the core's destination filter, so mDNS does not work.

Memory: lwIP + mbedtls are ~250 KB of code. `mcu/rv32/vixen_net.ld` keeps the
whole interpreter in BRAM as before (~7.5 KB left) and moves only the network
libraries to chip RAM (`.chiptext`, a second `.rvk` block), pulling the
per-packet lwIP code and AES/GCM/SHA-256 back into BRAM. The LVGL build's
layout already puts them in chip RAM. C stack 16 KB (a TLS handshake measured
~4 KB). `NET=0` builds the previous network-less firmware.

Developed against a functional emulator ([emulator.md](emulator.md)) (Unicorn running `firmware.elf`, the Ethernet
registers bridged to QEMU user-mode networking): DHCP, static setup from a
config file, DNS, HTTP, HTTPS, `mip.install()` from micropython.org and
GitHub, the RMII edge detection -- both builds. It says nothing about speed:
on the real machine expect a TLS handshake to take seconds (bignum code runs
from chip RAM).

## Sound (SOUND=1, on in every build with LVGL or the network)

    import sound
    sound.wav.play("file.wav")             # loop=True repeats
    sound.wav.stop()
    sound.sid.play("file.sid")              # song=n picks a sub-tune
    sound.sid.stop()
    sound.wav.playing(), sound.sid.playing()

`play()` also takes a bytes-like object. Both run at the same time (PCM
engine and SID are separate). Soft reset silences them.

* **WAV** (`vixen_sound.c`): read completely into chip RAM, then played by
  the PCM engine on its own -- no CPU time while it plays. Integer PCM 8/16/
  24/32 bit, mono or stereo, any rate; above 48 kHz decimated (the engine
  plays at most one sample per 48 kHz mix tick). Stereo is voices 0/1 with
  the engine's 75/25 pan. Room: ~4.4 MB, i.e. ~47 s of 48 kHz 16-bit mono;
  a file whose 16-bit copy does not fit is stored as 8 bit (twice as long),
  beyond that `ValueError: WAV too long`.
* **SID** (`vx_sidplay.h` on the 6502 core `vx6502.h`): PSID and RSID. The
  tune's init and play routine/interrupt handler run on a 64 KB C64 image;
  raster compare and CIA 1 timer A are modelled for the timing, the I/O area
  otherwise only as far as tunes poke it. Frequencies are rescaled from the
  C64 clock (PAL 0.985 MHz / NTSC 1.023 MHz) to the VIXEN SID's 1 MHz.
  Control register writes are spaced 2 us apart so gate/test edges are not
  lost in the SID's 290 ns register replay. Called from `uart_poll()` like the
  Ethernet pump (no interrupts). A call runs 4 ms ahead of its time; every
  SID write is stamped with its C64 cycle and queued, and `uart_poll()` gives
  it to the SID when the clock (raster line + HPOS, 0.25 us units) gets there
  -- so writes are spread over the frame as on a C64 and poll jitter does not
  move the beat. `$D011/$D012` and the CIA 1 timer A counter follow the same
  virtual clock. A C call that blocks longer than the lead makes the tune
  stumble. Not supported: NMIs (CIA 2 digis), 2SID/3SID, BASIC tunes.

Memory: nothing in BRAM except the qstrs, the module table entry and the
`uart_poll()` hook -- +176 bytes in the network build (7416 bytes left),
+40 in the LVGL build. `vixen_sound.o` is excluded from BRAM in
`vixen_net.ld` (the LVGL layout puts it in chip RAM anyway) and compiled with
`-msmall-data-limit=0 -O2`; its state is in `.bss` (chip RAM). Audio data and
the C64 image use the chip-RAM window 0x380000..0x7F0000 (the linker scripts
assert that nothing else grows into it). Avoid 64-bit division in this code:
it links libgcc's `__umoddi3`/`__udivdi3`, and libgcc is in BRAM.
While nothing plays the cost is one byte test in `uart_poll()`; a SID tune
costs an estimated 3-5 % CPU (instruction counts in the emulator, not
measured on the hardware).

Tests: `tools/sidtest/cputest.c` runs Klaus Dormann's 6502 functional test
(incl. decimal mode) on the core; `tools/sidtest/sidplay.c` plays a .sid
through the same player code on the PC and prints what reaches the SID.
`VX_SOUNDLOG=file` makes `tools/emu/vxemu.py` log SID/PCM register writes
and dump the sound window.

## Timebase

The Murax core implements only the `mstatus`/`mie`/`mcause`/`mip` CSRs -- there
is no `mcycle`/`mtime`, so `rdcycle`/`rdtime` cannot be used. `mcu/riscv/ticks.c`
instead accumulates the VIH raster line counter (`VIH_RASTER_VPOS`, 0..311; one
line = 64 us, one frame = 19.968 ms) into a 64-bit line count that backs
`ticks_ms()`/`ticks_us()`. It must be sampled at least once per frame, which
`uart_poll()` does (from the input wait, `time.sleep()` and the VM hook). C code
that runs longer than ~20 ms without returning to the VM (e.g. a huge bignum
operation or a large gc collect) makes the clock lag by the missed frames; it
never runs fast.

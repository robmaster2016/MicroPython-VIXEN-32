# TODO

New in VIXEN-32 core v0.77-v0.98 that MicroPython could use, most useful
first. Register names are from the v0.98 devkit (`common/vih_regs.h`,
`common/kernel_api.h`).

- [x] **Zorro RAM as GC heap** -- 48 MB of SDRAM after chip RAM
  (`ZORRO_RAM_BASE`, `vih_zorro_present()`, core v0.89+), CPU only, as fast as
  chip RAM. Add it as a second GC area (`MICROPY_GC_SPLIT_HEAP`) when present;
  fall back to the current chip-RAM arena on older cores.
- [x] **Trapdoor RAM + instruction cache** -- 256 KB at `0x80000`
  (`TRAPDOOR_RAM_BASE`, `vih_trapdoor_present()`, v0.87+) plus the 64 KB
  8-bit-CPU RAM at `0xC0000` while no 6502/Z80 runs. With `vih_icache(1)`
  (v0.88) code there runs at ~99 % of fast-RAM speed: room for code that does
  not fit the 423 KB BRAM (LVGL's hot paths).
  Done: the icache is on (compile -37 %), LVGL's 64 KB colour table moved to
  Trapdoor RAM and the compiler into the freed BRAM (compile -44 % in all).
  Not used yet: the rest of Trapdoor RAM (~190 KB) and the 8-bit CPU RAM.
- [x] **80 x 24 hi-res text console** -- `vih_video_mode(1)` (v0.95): the tile
  layer at 768 dots a line, map width `VIH_HIRES_MAP_W`. A wider REPL, maybe as
  an option (`vixen.console_mode()`).
  Done: 92 x 27 by default, CAPS LOCK switches to 46 x 27,
  `vixen.console_hires()`, `vixen.console_size()`.
- [x] **`machine` additions** -- core version (`VIH_CORE_VERSION`, $98 on
  v0.98, 0 on older cores) and FPGA temperature (`vih_temp_mc()`,
  `vih_temp_valid()`, v0.82).
  Done in the `vixen` module: `core_version()`, `kernel_version()`,
  `temperature()`.
- [x] **New blitter features for graphics modules** -- Z buffer
  (`vih_blit_z`, v0.90), perspective textures (v0.91), whole triangles
  (`vih_tri.h`, v0.93), op queue (`vih_blit_queue`, v0.85), 8 x 256 palettes
  (`vih_pal_sel`, v0.95).
  Done: the `gfx` module (docs/gfx.md) -- triangles, Z buffer, perspective,
  queue, textures, the 8 palettes, the colour LUT and base.
- [x] **Native code emitters** -- `@micropython.native`/`viper`/`asm_rv32`
  are off: MicroPython's RV32 emitter always emits compressed (C extension)
  instructions, which the core lacks. Needs an emitter option for plain
  rv32im (py/asmrv32.c: every `asm_rv32_opcode_c*` use behind a flag), then
  `MP_PLAT_COMMIT_EXEC` with `vih_cache_invalidate()` (icache is on).
  Done: patches/micropython-rv32-norvc.patch, MICROPY_EMIT_RV32_RVC = 0.

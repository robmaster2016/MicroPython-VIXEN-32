# SPDX-License-Identifier: MIT
# VIXEN-32 board configuration (MicroPython ports/vixen).
#
# VIXEN-32 is a 32-bit RISC-V home computer (VexRiscv "Murax" core, 100 MHz)
# that runs as a core on the MEGA65. Code runs from the 512 KB on-chip BRAM
# (plus a chip-RAM code block in the network/LVGL builds); bss, heap, stack
# and the gc arena live in the 16 MB chip-RAM window (docs/architecture.md).

VIXEN_ARCH = riscv32

# Match the VIXEN devkit exactly: rv32im_zicsr / ilp32 (no C extension, no F).
RV32_ARCH  = rv32im_zicsr
RV32_ABI   = ilp32

# Board/MCU names. MICROPY_HW_BOARD_NAME is set by the port Makefile from
# $(BOARD); the MCU name comes here (mirrors the qemu rv32 board's approach).
CFLAGS += -DMICROPY_HW_MCU_NAME='"$(RV32_ARCH)"'

# Flags for mpy-cross when freezing .py (FROZEN_MANIFEST). mpy-cross only knows
# rv32imc as a RISC-V native target, and VIXEN has NO C extension, so no
# -march is passed: frozen modules are plain bytecode (none of them uses
# @native), which runs on any core.
MPY_CROSS_FLAGS =

# Memory budget. The gc arena and C stack live in the 16 MB chip-RAM window
# (see vixen.ld .gcram / .Stack / main.c), so they can be large; only the
# interpreter image (code+rodata+data+bss+libc heap) sits below them in the
# same window. 1 MB gc arena + 8 KB C stack is comfortable headroom.
MICROPY_HEAP_SIZE ?= 1048576     # 1 MB MicroPython gc arena (in chip-RAM)
MICROPY_STACK_SIZE ?= 8192       # C call stack (in chip-RAM, grows down)

# NOTE: VFS is NOT toggled here. MICROPY_VFS / MICROPY_READER_VFS are C
# preprocessor defines (set in mpconfigport.h), not make variables -- nothing
# in the build converts a MICROPY_* make var into a -D flag. The VFS framework
# is linked so the file-reader/import symbols resolve; vixen_disk.c mounts the
# kernel's disk units (/sd, /a, /b) at startup.

# GC register/stack walker. gchelper_native.o (C) provides
# gc_helper_collect_regs_and_stack; gchelper_rv32i.s (asm) provides
# gc_helper_get_regs_and_sp. Matches the qemu VIRT_RV32 board.
SRC_BOARD_O += shared/runtime/gchelper_native.o shared/runtime/gchelper_rv32i.o

# Linker script for this board.
LDSCRIPT = mcu/rv32/vixen.ld

# Frozen modules: none for the plain build (the network build sets
# boards/manifest_net.py in the Makefile).
FROZEN_MANIFEST ?=

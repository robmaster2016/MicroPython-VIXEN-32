# SPDX-License-Identifier: MIT
# VIXEN-32 port of MicroPython.
#
# Builds firmware.elf in DOOM-style layout (see mcu/rv32/vixen.ld): code
# (text/rodata/data) in on-chip BRAM at 0x100, and the interpreter's working
# memory (bss/heap/stack/gc arena) in the 16 MB cached chip-RAM window at
# 0x40000000. The CPU runs code from BRAM and reads/writes the heap through the
# chip-RAM window. Toolchain: the VIXEN xPack riscv-none-elf cross compiler
# (rv32im_zicsr / ilp32).
#
#   make                      -> build-<board>/firmware.elf (+ .bin + .rvk)
#   make CROSS_COMPILE=/path/bin/riscv-none-elf-   -> use an explicit toolchain
#
# The .elf is wrapped into a loadable .rvk (single BRAM code block) with the
# VIXEN devkit's mkrvpk; load it from VIXEN-32's F12/Help "Load RVK" menu.
# VIXEN-32 itself is untouched (no BOOT.BIN, no trampoline). See docs/building.md.

BOARD ?= VIXEN32
BOARD_DIR ?= boards/$(BOARD)

ifeq ($(wildcard $(BOARD_DIR)/.),)
$(error Invalid BOARD specified: $(BOARD_DIR))
endif

BUILD ?= build-$(BOARD)

# MicroPython v1.29.0 checkout (fetched by setup.sh into deps/). The port is
# built out of tree: everything below refers to it through $(TOP).
MPY_DIR ?= ../../deps/micropython
include $(MPY_DIR)/py/mkenv.mk
-include mpconfigport.mk

# Board-specific settings (toolchain arch, heap/stack, LDSCRIPT, ...).
include $(BOARD_DIR)/mpconfigboard.mk

# qstr definitions (must come before including py.mk)
QSTR_DEFS = qstrdefsport.h

MICROPY_ROM_TEXT_COMPRESSION ?= 1

# MicroPython feature configurations
MICROPY_FLOAT_IMPL ?= float

# LVGL via lv_binding_micropython, built as a user C module (README, "LVGL").
# deps/usermods/lvgl links to the binding checkout (deps/lv_binding_micropython,
# with its lvgl submodule at the version the binding expects, 9.3.0), set up by
# setup.sh. LVGL=0 builds the plain interpreter.
LVGL ?= 0
ifeq ($(LVGL),1)
USER_C_MODULES ?= $(abspath ../../deps/usermods)
LV_CONF_PATH ?= $(abspath lv_conf.h)
# Code split BRAM / chip RAM (see the linker script), more gc heap for LVGL
# objects and decoded images. The C stack is in BRAM (vixen_lvgl.ld): 16 KB,
# LVGL's deepest path measured ~5.7 KB.
LDSCRIPT = mcu/rv32/vixen_lvgl.ld
MICROPY_HEAP_SIZE = 4194304
MICROPY_STACK_SIZE = 16384
SRC_C += vx_lvgl.c vx_lvgl_mp.c
CFLAGS += -DVIXEN_LVGL=1
# Decoded-image cache (lv_conf.h LV_CACHE_DEF_SIZE): without it LVGL decodes a
# PNG again on every redraw.
CFLAGS += -DMICROPY_CACHE_SIZE=524288
endif

# Network (README, "Network"): lwIP on the core's Ethernet MAC (vixen_eth.c)
# gives network.LAN and socket; mbedtls gives ssl; mip and requests are frozen
# in, so `mip.install()` works out of the box. lwIP + mbedtls are ~250 KB of
# code, more than the BRAM has left: mcu/rv32/vixen_net.ld keeps the
# interpreter in BRAM and moves the network libraries to chip RAM (the LVGL
# build's layout already does that). NET=0 builds without any of it.
NET ?= 1
ifeq ($(NET),1)
MICROPY_PY_LWIP = 1
MICROPY_PY_SSL = 1
MICROPY_SSL_MBEDTLS = 1
INC += -Ilwip_inc
SRC_C += vixen_eth.c
# curve25519 module (X25519 + Ed25519, for SSH): chip RAM, -O2 (the field
# multiply is the whole cost), state out of the BRAM .sbss.
SRC_C += vixen_25519.c
$(BUILD)/vixen_25519.o: CFLAGS += -msmall-data-limit=0 -O2
# ...and its BRAM stack also carries AES-CTR, AES-GCM and SHA-256 (sshd, TLS):
# every call of these goes through a __wrap_ in vixen_25519.c.
LDFLAGS += -Wl,--wrap=mbedtls_aes_crypt_ctr,--wrap=mbedtls_gcm_update,--wrap=mbedtls_sha256_update,--wrap=mbedtls_sha256_finish
ifeq ($(FROZEN_MANIFEST),)
FROZEN_MANIFEST = $(abspath boards/manifest_net.py)
endif
ifneq ($(LVGL),1)
LDSCRIPT = mcu/rv32/vixen_net.ld
# A TLS handshake measured ~4 KB of C stack (emulator, mip.install over
# HTTPS) on top of whatever Python call depth leads to it: 16 KB, not 8.
MICROPY_STACK_SIZE = 16384
endif
endif

# Code in two blocks (BRAM + chip RAM) when LVGL or the network is built in.
SPLIT_IMAGE = $(filter 1,$(LVGL) $(NET))

# Sound (README, "Sound"): the `sound` module -- WAV on the PCM engine, PSID
# player on the SID (vixen_sound.c, 6502 core vx6502.h). Code, constants and
# state all live in chip RAM (.chiptext/.bss; -msmall-data-limit=0 keeps even
# small globals out of the BRAM .sbss), so it needs a build with a chip-RAM
# code block: on by default whenever LVGL or the network is built in.
ifneq ($(SPLIT_IMAGE),)
SOUND ?= 1
else
SOUND ?= 0
endif
ifeq ($(SOUND),1)
SRC_C += vixen_sound.c
CFLAGS += -DVIXEN_SOUND=1
# -O2: the 6502 core is the hot loop while a tune plays; costs chip RAM only.
$(BUILD)/vixen_sound.o: CFLAGS += -msmall-data-limit=0 -O2
endif

# Randomness (os.urandom, TLS, lwIP): chip RAM, state out of the BRAM .sbss.
$(BUILD)/vixen_random.o: CFLAGS += -msmall-data-limit=0

# include py core make definitions
include $(TOP)/py/py.mk
include $(TOP)/extmod/extmod.mk

CFLAGS += -DMICROPY_HEAP_SIZE=$(MICROPY_HEAP_SIZE) -DMICROPY_STACK_SIZE=$(MICROPY_STACK_SIZE)

# ---------------------------------------------------------------------------
# RISC-V 32-bit (VIXEN-32) toolchain and link
# ---------------------------------------------------------------------------

# Override with: make CROSS_COMPILE=/path/to/bin/riscv-none-elf-
# (../../build.sh sets up the toolchain used for the releases.)
CROSS_COMPILE ?= riscv-none-elf-

# Host compiler for build tools (mkrvpk). The cross toolchain builds the target
# objects; the host tools run on the build machine.
CC_HOST ?= cc

RV32_ARCH  = rv32im_zicsr
RV32_ABI   = ilp32

AFLAGS += -mabi=$(RV32_ABI) -march=$(RV32_ARCH)
CFLAGS += $(AFLAGS)
LIBS   = $(shell $(CC) $(CFLAGS) -print-libgcc-file-name) -lm

# Link through the compiler driver so gcc-style flags (-nostartfiles, -T,
# --gc-sections) work and libc/libgcc resolve.
LD := $(CC)

# -nostartfiles: skip newlib's crt0 so our own _start (mcu/rv32/entrypoint.s)
# runs first; libc (printf/malloc) and libgcc are still linked. Linker-only
# options go through -Wl, so the gcc driver forwards them to ld.
LDFLAGS += -nostartfiles -Wl,-T,$(LDSCRIPT) -Wl,--gc-sections -Wl,-Map=$(@:.elf=.map)
# The linker script sizes the chip-RAM gc arena (.gcram) from MICROPY_HEAP_SIZE
# and the C call stack reservation (.Stack) from MICROPY_STACK_SIZE; both must
# match the values the C code sees (CFLAGS above). Define them for ld.
LDFLAGS += -Wl,--defsym,MICROPY_HEAP_SIZE=$(MICROPY_HEAP_SIZE)
LDFLAGS += -Wl,--defsym,MICROPY_STACK_SIZE=$(MICROPY_STACK_SIZE)

SRC_C += \
	mcu/rv32/interrupts.c \
	mcu/rv32/startup.c \
	mcu/riscv/ticks.c \
	syscalls.c \
	main.c \
	uart.c \
	mphalport.c \
	vfs_rom_ioctl.c \
	vixen_disk.c \
	vixen_random.c \
	shared/libc/string0.c \
	shared/readline/readline.c \
	shared/runtime/interrupt_char.c \
	shared/runtime/pyexec.c \
	shared/runtime/stdout_helpers.c \
	shared/runtime/sys_stdio_mphal.c \

SRC_BOARD_O += mcu/rv32/entrypoint.o

# ---------------------------------------------------------------------------
# Common build settings
# ---------------------------------------------------------------------------

INC += -I.
INC += -I$(TOP)
INC += -I$(BUILD)
# VIXEN-32 devkit register definitions (vih_regs.h: VIH text/tile layer,
# keyboard FIFO, blitter) used by uart.c for the console.
# Not part of this repository: get the devkit with the VIXEN-32 core release
# and pass its path (build.sh: VIXEN_DEVKIT=...).
DEVKIT_DIR ?= ../../deps/vixen32-devkit
ifeq ($(wildcard $(DEVKIT_DIR)/common/vih_regs.h),)
$(error VIXEN-32 devkit not found in $(DEVKIT_DIR): set DEVKIT_DIR (or VIXEN_DEVKIT for build.sh))
endif
INC += -I$(DEVKIT_DIR)/common

CFLAGS += -DMICROPY_HW_BOARD_NAME='"$(BOARD)"'
CFLAGS += $(INC) -Wall -Wpointer-arith -Wdouble-promotion -Wfloat-conversion \
	-ffunction-sections -fdata-sections -std=gnu99 $(COPT)

# Debugging/Optimization
ifeq ($(DEBUG), 1)
CFLAGS += -g
COPT = -O0
else
COPT += -Os -DNDEBUG
endif

# ---------------------------------------------------------------------------
# Sources, objects, qstr extraction
# ---------------------------------------------------------------------------

OBJ += $(PY_O)
OBJ += $(addprefix $(BUILD)/, $(SRC_C:.c=.o))
OBJ += $(addprefix $(BUILD)/, $(SRC_BOARD_O))
OBJ += $(addprefix $(BUILD)/, $(LIBM_SRC_C:.c=.o))

SRC_QSTR += $(SRC_C) $(LIBM_SRC_C)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------

all: $(BUILD)/firmware.elf $(BUILD)/firmware.bin $(BUILD)/firmware.rvk

$(BUILD)/firmware.elf: $(LDSCRIPT) $(OBJ)
	$(Q)$(LD) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)
	$(Q)$(SIZE) $@

# BRAM block only (the LVGL build has a second, chip-RAM block: .chiptext).
$(BUILD)/firmware.bin: $(BUILD)/firmware.elf
	$(Q)$(OBJCOPY) -O binary -j .text $^ $@

$(BUILD)/firmware-chip.bin: $(BUILD)/firmware.elf
	$(Q)$(OBJCOPY) -O binary -j .chiptext $^ $@

# Tests: tests/run_tests.sh in the repository runs the firmware.elf in the
# functional emulator (tools/emu/vxemu.py, docs/testing.md).

# ---------------------------------------------------------------------------
# Loadable app: firmware.rvk  (a normal VIXEN-32 .rvk, loaded by the F12/Help
# "Load RVK" menu -- VIXEN-32 itself is left untouched: no BOOT.BIN, no
# trampoline).
#
# The image is DOOM-style: code (text/rodata/data) in on-chip BRAM at 0x100,
# the interpreter's bss/heap/stack/gc in the chip-RAM window (0x40000000),
# zeroed at runtime. objcopy -O binary therefore emits ONLY the BRAM code block
# (the NOLOAD heap/gc/stack carry no bytes), so the .rvk is a single BRAM block
# -- exactly like apptest.rvk / display_test.rvk.
#
#   make                -> build-$(BOARD)/firmware.elf (+ .bin + .rvk)
#   make firmware.rvk
#
# The .rvk carries code only; the chip-RAM heap is zeroed by entrypoint.s (bss)
# and main.c (gc arena) once VIXEN-32 jumps to _start == 0x100.
# ---------------------------------------------------------------------------

# VIXEN devkit source tree (holds tools/mkrvpk.c and a prebuilt mkrvpk host
# binary). Defined above with the include path; override if it moved.
MKRVPK     ?= $(DEVKIT_DIR)/tools/mkrvpk

ifneq ($(SPLIT_IMAGE),)
$(BUILD)/firmware.rvk: $(BUILD)/firmware.bin $(BUILD)/firmware-chip.bin
	$(Q)$(MKRVPK) $@ 0x00000100 bram:0x00000100:$(BUILD)/firmware.bin \
		sdram:0x00100000:$(BUILD)/firmware-chip.bin
else
$(BUILD)/firmware.rvk: $(BUILD)/firmware.bin
	$(Q)$(MKRVPK) $@ 0x00000100 bram:0x00000100:$(BUILD)/firmware.bin
endif

include $(TOP)/py/mkrules.mk

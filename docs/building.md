# Building and installing

## What you need

* Linux (any distribution; macOS should work but is untested)
* `git`, `make`, a host C compiler, Python 3
* An rv32 GCC with a multilib for rv32i **without** the C extension (the
  VIXEN core has no compressed instructions). The releases are built with
  Espressif's `riscv32-esp-elf` GCC 14.2 (`esp-14.2.0_20241119`), which ships
  an `rv32i_zicsr_zifencei/ilp32` multilib. Install it with Espressif's
  `idf_tools.py` or unpack the release archive anywhere.
* The **VIXEN-32 devkit** (`vixen32-devkit.zip`, part of the VIXEN-32 core
  release). The port uses its register and kernel API headers
  (`common/vih_regs.h`, `common/kernel_api.h`) and `tools/mkrvpk` to pack the
  `.rvk`. It is not part of this repository.
* For the tests: `qemu-system-x86_64` (only its user-mode network is used)
  and OpenSSH for the SSH test. See [testing.md](testing.md).

## Fetch the dependencies

    VIXEN_DEVKIT=/path/to/vixen32-devkit ./setup.sh

This puts everything into `deps/` (ignored by git):

| Path | What |
| --- | --- |
| `deps/micropython` | MicroPython v1.29.0 with the lwip, mbedtls and micropython-lib submodules |
| `deps/lv_binding_micropython` | LVGL binding (pinned commit, LVGL 9.3.0), patched with `patches/lv_binding-vixen.patch` |
| `deps/usermods/lvgl` | link to the binding, used as `USER_C_MODULES` |
| `deps/vixen32-devkit` | link to your devkit |

`mpy-cross` is built once by `setup.sh` (it freezes `mip`, `net` and `sshd`
into the firmware). Running `setup.sh` again leaves existing checkouts alone.

## Build

    ./build.sh BUILD=build-net                  # network + sound + SSH
    ./build.sh LVGL=1 BUILD=build-lvgl-net      # the same plus LVGL

If the toolchain is not in `~/.espressif/tools/...`, point to its `bin/`:

    VIXEN_TOOLCHAIN=/opt/riscv32-esp-elf/bin ./build.sh BUILD=build-net

| Build | Command | Contents |
| --- | --- | --- |
| network | `./build.sh BUILD=build-net` | REPL, SD card, network, TLS, `mip`, `requests`, `sshd`, `sound`, `curve25519` |
| LVGL + network | `./build.sh LVGL=1 BUILD=build-lvgl-net` | everything above plus `lvgl`, 4 MB gc heap, C stack in BRAM |
| LVGL only | `./build.sh LVGL=1 NET=0 BUILD=build-lvgl` | LVGL and sound, no network |
| plain | `./build.sh NET=0 BUILD=build-plain` | the interpreter alone, BRAM only, single-block `.rvk` |

The result is `ports/vixen/<BUILD>/firmware.rvk`, next to `firmware.elf`
(for the emulator), `firmware.bin` and the link map.

The LVGL + network build is the most complete one and the recommended
default; the network build leaves more room in chip RAM and keeps the
C stack in chip RAM (only ~2 KB of BRAM are left in it).

### Why the special link step

The port's objects are compiled for `rv32im_zicsr`, but a GCC driver links
its *default* multilib libc unless told otherwise -- usually `rv32imac`, and
a single compressed instruction crashes the core. `build.sh` therefore links
with `-march=rv32i_zicsr_zifencei` and passes the matching libgcc. Check a
firmware with

    riscv32-esp-elf-readelf -A ports/vixen/build-net/firmware.elf

`Tag_RISCV_arch` must not contain `c2p0`. The Espressif newlib has only a
weak `__getreent()` stub; `syscalls.c` provides a real one.

After adding or renaming qstrs in frozen modules, delete
`<BUILD>/frozen_content.c` if the build reports a "redeclaration of
MP_QSTR_...".

## Install on the VIXEN-32

1. Copy `firmware.rvk` to the SD card, for example to
   `/vixen-32/apps/micropython/firmware.rvk`.
2. Copy the example scripts you want from `ports/vixen/examples/` to
   `/vixen-32/apps/micropython/examples/`. Your own modules go to
   `/vixen-32/apps/micropython/lib/`. Both folders are on `sys.path`.
3. Optional autostart: `/vixen-32/apps/micropython/boot/boot.py` and
   `boot/main.py` run at start-up and after every soft reset (Ctrl-D).
4. On the VIXEN-32 open **F12/Help -> Load RVK** and pick `firmware.rvk`.

The SD card unit (drive C:) must be enabled in the installed `BOOT.BIN`,
otherwise `/sd` does not appear (disk images in A: and B: work regardless).
See [architecture.md](architecture.md#sd-card-and-disk-images).

Network setup: [network.md](network.md). SSH: [ssh.md](ssh.md).
LVGL: [lvgl.md](lvgl.md). Sound: [sound.md](sound.md).

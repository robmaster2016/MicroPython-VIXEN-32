# The functional emulator (vxemu)

`ports/vixen/tools/emu/vxemu.py` runs a `firmware.elf` on the Unicorn CPU
emulator (rv32im) and models just enough of the VIXEN-32 to develop and test
the port on a PC:

* BRAM and the 16 MB chip-RAM window
* the raster counter as the timebase (follows the wall clock)
* the Ethernet MAC: registers and packet windows, 32-bit access enforced. The
  frames are bridged over UDP to a QEMU user-mode network (DHCP gives
  10.0.2.15, DNS 10.0.2.3, NAT to the internet).
* the console: `uart_tx_strn`, `uart_rx_chr` and `uart_rx_any` are
  intercepted; the script is sent through the raw REPL.

Not modelled: the SD card (the kernel's disk table is empty -- prepend
`sd_ramfs.py` to a script for a RAM `/sd` that behaves like the real one),
keyboard, mouse, sprites, audio output. The display is optional (`VX_SCREEN`,
below). It says nothing about speed:
use it for function, measure speed on the machine.

## Setup

    python3 -m venv ports/vixen/tools/emu/venv
    ports/vixen/tools/emu/venv/bin/pip install unicorn

(`tests/run_tests.sh` does this on its first run.) QEMU must provide the
`user`, `dgram` and `hubport` netdevs (`qemu-system-x86_64`, any recent
version). `vxemu.py` looks for `riscv32-esp-elf-nm` in `VIXEN_TOOLCHAIN`, the
default Espressif path and `PATH`; `VX_NM` overrides it.

## Usage

    E=ports/vixen/tools/emu
    $E/venv/bin/python $E/vxemu.py ports/vixen/build-net/firmware.elf script.py [timeout_s]

The script's output is printed; at the end the emulator reports the Ethernet
frame counts, the C stack high-water mark and the curve25519 BRAM stack use.

| Variable | Effect |
| --- | --- |
| `VX_FWD="tcp:127.0.0.1:2222-:22 ..."` | forward host ports to the board (for servers) |
| `VX_STAY=1` | after the script, stay in the normal REPL until the timeout (servers that run in the background on `os.dupterm`) |
| `VX_LATE=1` | only the late RMII sampling edge works (tests the edge detection) |
| `VX_PROF="sym,..."` | profile calls of these C functions: instructions from BRAM / chip RAM, loads and stores per region, a cycle estimate (`VX_PROF_W/_R/_I` are the model constants) |
| `VX_SCREEN=1` | model the display: the console draws into the tile map, LVGL into the framebuffer, the blitter runs FILL, COPY and LINE. Needs `VX_FONT` |
| `VX_FONT=.../rtl/font_rom.hex` | the 8x8 ROM font from the VIXEN-32 core source (the MEGA65 ASCII font by deft), for text screens |
| `VX_SHOT="a.png,b.png"` | with `VX_SCREEN`: each `machine.mem32[0x3A1FFFF0] = 0` in the script saves the next screenshot (384 x 224 PNG) |
| `VX_SHOT_END=file.png` | with `VX_SCREEN`: a screenshot when the script is done |
| `VX_TYPE=1` | type the script into the normal REPL line by line, as a user would (echo and prompts appear on the screen) |
| `VX_SETUP=file` | with `VX_TYPE`: run this script through the raw REPL first, e.g. to fill the RAM `/sd` |
| `VX_SOUNDLOG=file` | log SID and PCM register writes as `time_s reg value`, dump the sound module's chip-RAM window to `file.bin` |

Example: the SSH server with a RAM SD card, reachable from the host:

    cat $E/sd_ramfs.py myscript.py > /tmp/board.py   # myscript: lan setup + sshd.start()
    VX_STAY=1 VX_FWD="tcp:127.0.0.1:2222-:22" $E/venv/bin/python $E/vxemu.py \
        ports/vixen/build-net/firmware.elf /tmp/board.py 600
    ssh -p 2222 vixen@127.0.0.1

`tests/test_sshd.sh` is a complete, working example of this.

## Screenshots

`docs/screenshots/make_screenshots.sh` makes every picture in
`docs/screenshots/` this way, at the native 384 x 224 plus a 2x copy
(nearest neighbour) for the README:

    VX_FONT=/path/to/vixen32-core/source/rtl/font_rom.hex docs/screenshots/make_screenshots.sh

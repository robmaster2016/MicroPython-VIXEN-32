# ports/vixen

The MicroPython port for the VIXEN-32. It builds out of tree against the
MicroPython checkout that `setup.sh` puts into `deps/`.

* Building and installing: [docs/building.md](../../docs/building.md)
* How the port works: [docs/architecture.md](../../docs/architecture.md)
* Emulator and tests: [docs/emulator.md](../../docs/emulator.md), [docs/testing.md](../../docs/testing.md)

| Path | What |
| --- | --- |
| `main.c`, `uart.c`, `mphalport.c` | start-up, console (VIH text layer + keyboard), HAL |
| `vixen_disk.c` | `vixen` module, SD card and disk images through the kernel API |
| `vixen_eth.c`, `lwip_inc/` | `network.LAN`: lwIP on the Ethernet MAC |
| `vixen_random.c` | SHA-256 DRBG behind `os.urandom` |
| `vixen_25519.c` | `curve25519` module (X25519, Ed25519) and BRAM-stack crypto wrappers |
| `vixen_sound.c`, `vx_sidplay.h`, `vx6502.h` | `sound` module: WAV and SID player |
| `vx_lvgl.c`, `vx_lvgl_mp.c`, `lv_conf.h` | LVGL display and mouse driver |
| `mcu/rv32/*.ld` | linker scripts: plain, network, LVGL |
| `modules/` | frozen Python: `net`, `sshd` |
| `examples/` | example scripts for the SD card |
| `tools/emu/` | functional emulator (`vxemu.py`, display model `screen.py`, RAM SD card `sd_ramfs.py`) |
| `tools/sidtest/` | PC tests for the 6502 core and SID player |
| `tools/sidtune/` | the example tune: score + 6502 player -> `.sid` (`compose.py`), PC preview (`preview.py`) |

# Testing

## Emulator test suite

    ./build.sh BUILD=build-net
    tests/run_tests.sh                       # all tests against build-net
    FW=ports/vixen/build-lvgl-net/firmware.elf tests/run_tests.sh
    OFFLINE=1 tests/run_tests.sh             # skip tests that need the internet
    tests/run_tests.sh test_crypto.py        # selected tests

Each `tests/test_*.py` runs on the firmware in the [emulator](emulator.md)
and passes when it prints `TEST OK` without a traceback; `test_*.sh` are
host-side tests that start the emulator themselves. Logs go to `tests/out/`.

| Test | Checks | Internet |
| --- | --- | --- |
| `test_core.py` | language features, standard modules, timebase, 512 KB allocations, every port module imports | no |
| `test_crypto.py` | SHA-256, AES-CTR (NIST SP 800-38A), `os.urandom`, X25519 (RFC 7748), Ed25519 (RFC 8032 + OpenSSL-made vectors), rejection of bad signatures and points | no |
| `test_lvgl.py` | display driver, widgets, rendering, PNG decoding (passes trivially without LVGL) | no |
| `test_net.py` | `net.up()` with DHCP, then a static setup from a config file, TCP | yes |
| `test_http.py` | DHCP, DNS, HTTP | yes |
| `test_https_mip.py` | HTTPS with `requests`, `mip.install()` from micropython.org and GitHub | yes |
| `test_examples.py` | `sysinfo`, `clock` (NTP, Berlin time), `weather` (Open-Meteo), `wget` (HTTPS to the SD card), their LVGL screens, `sound_demo` with the SID; the examples are put on a RAM `/sd` first | yes |
| `test_sshd.sh` | `sshd` with the host's OpenSSH: ed25519 key login, `scp -O` upload, sftp download, byte-for-byte compare | no |

Run the suite against both network builds before a release.

## PC tests for the sound engine

`ports/vixen/tools/sidtest/` builds the 6502 core and the SID player for the
PC: `cputest` runs Klaus Dormann's 6502 functional test, `sidplay` plays a
`.sid` and reports call rates and note starts. See its README.

## On the machine

The emulator proves function, not speed or real hardware behaviour. Each
guide ends with a short "Quick check" to run on the VIXEN-32:
[lvgl.md](lvgl.md), [network.md](network.md), [sound.md](sound.md),
[ssh.md](ssh.md). `examples/netbench.py` with `ports/vixen/tools/netbench_host.py`
measures raw TCP, SD card and network-to-SD throughput.

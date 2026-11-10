# Networking

The network builds of the VIXEN-32 MicroPython port have their own TCP/IP stack
(lwIP on the core's Ethernet MAC) and TLS (mbedtls). They provide
`network.LAN`, `socket`, `ssl`, and the frozen modules `mip`, `requests`, `net`
and [`sshd`](ssh.md).

## Firmware

The network is part of the default build (see [Building](building.md)):

```sh
./build.sh BUILD=build-net                    # MicroPython + network + sound
./build.sh LVGL=1 BUILD=build-lvgl-net        # MicroPython + LVGL + network + sound
```

`NET=0` builds the firmware without network. Copy
`ports/vixen/<BUILD>/firmware.rvk` to the SD card and start it from the F12/Help
menu.

## Getting connected

You do **not** need SHIFT+POUND from BASIC: MicroPython brings up the network
card itself.

```python
import net
lan = net.up()       # reads /vixen-32/NETWORK.TXT like BASIC
```

Output, as in BASIC:

```
ETHERNET ON  MAC 02:56:33:32:00:01
LINK UP
IP 192.168.1.50  GW 192.168.1.1  DNS 192.168.1.1
```

`NETWORK.TXT` works as in BASIC: `ip: dhcp`, or `ip:`, `mask:`, `gw:` and
`dns:` for a static address, optionally `mac:`. For a static setup the
defaults are mask `255.255.255.0`, gateway `0.0.0.0` and DNS `8.8.8.8`.
Without a `mac:` line the same MAC as in BASIC is used, so the router usually
hands out the same IP.

`net.up(timeout=20)` raises `OSError` if there is no link ("is the cable in?")
or no DHCP answer within the timeout (seconds).

To bring the network up automatically at startup, put this in
`boot/boot.py` (in `/vixen-32/apps/micropython` on the card):

```python
import net
net.up()
```

## Installing packages with mip

```python
import mip
mip.install("base64")                      # from micropython-lib
mip.install("github:user/repo/file.py")    # straight from GitHub
```

Packages go to `/sd/vixen-32/apps/micropython/lib` and can be imported right
away.

## More examples

```python
import requests
r = requests.get("https://micropython.org/pi/v2/index.json")
print(r.status_code, len(r.content))

import socket
s = socket.socket()
s.connect(socket.getaddrinfo("vixen-32.mooo.com", 2323)[0][-1])
print(s.recv(200))
```

Diagnostics:

* `lan.status()`: 0 no cable, 1 link, 2 waiting for DHCP, 3 has an IP
* `lan.status("stats")`: (frames received, frames sent, hardware drops, CRC errors)
* `lan.ifconfig()`: (ip, mask, gateway, dns)

## Good to know

* The hardware buffers only 4 received frames. The TCP window is tuned for
  that; a Python program does not need to do anything. The network runs in the
  background, also at the REPL and during `time.sleep()`.
* Speed, measured on the real machine with a PC on Wi-Fi:

  | Transfer | Speed |
  |---|---|
  | raw TCP to the VIXEN | ~280 KB/s |
  | raw TCP from the VIXEN | ~410-550 KB/s |
  | SD card write | ~150 KB/s |
  | scp / sftp ([SSH](ssh.md)) | ~36-62 KB/s |

  Measure it yourself: copy `ports/vixen/examples/netbench.py` to the card and
  run `import netbench; netbench.run()` on the VIXEN, then on the PC
  `python3 ports/vixen/tools/netbench_host.py <ip>`. It separates network, SD
  card and network-to-SD.
* The LVGL firmware keeps the C stack in BRAM (faster, 16 KB): Python
  recursion goes about 48 levels deep before
  `RuntimeError: maximum recursion depth exceeded`.
* HTTPS: setting up a connection takes a few seconds (cryptography at 100 MHz).
* Certificates are not verified (MicroPython default), and there is no clock
  to check expiry dates.
* Randomness (`os.urandom`, TLS): a SHA-256 based generator fed from keyboard
  and packet timing, the raster beam and a seed file on the SD card (see
  below). Without a real hardware random source this is decent, not certified.
* Multicast does not pass the hardware filter, so there is no mDNS
  (`vixen32.local`).
* If the connection does not work at all and `lan.status("stats")` shows many
  CRC errors, try `lan.config(rx_late=1)` (or `0`) before `net.up()`. Normally
  the driver detects this itself.

## Building blocks for servers

These are what [`sshd`](ssh.md) is built on, and you can use them for your own
servers.

### Random numbers

* **`os.urandom(n)`** returns n random bytes. The pool is fed from the timing
  of every key press and every received packet, the raster beam, and the file
  `random.seed` in the MicroPython folder on the SD card: it is read at
  power-on, mixed in and replaced with fresh bytes, so every session starts
  with what all previous ones collected. Create long-lived keys (such as an SSH
  host key) only once the seed file exists, i.e. from the second start on, or
  after typing a few lines.
* **`vixen.add_entropy(bytes)`** mixes in your own measurements.
* **`vixen.seed_file()`** rewrites the seed file immediately, e.g. after a
  login, so newly collected entropy is saved.

### AES-CTR

`cryptolib.aes(key, cryptolib.MODE_CTR, counter16)` works on data of any size,
also in consecutive chunks (`aes128-ctr`/`aes256-ctr` in SSH). The constants
`cryptolib.MODE_ECB`, `MODE_CBC` and `MODE_CTR` are available.

### Module `curve25519`

Key exchange and signatures for SSH (`curve25519-sha256`, `ssh-ed25519`),
written in C. Private keys are 32 bytes from `os.urandom(32)`.

* `curve25519.x25519(priv)` returns your public key;
  `curve25519.x25519(priv, peer_pub)` returns the shared secret (raises
  `ValueError` if it is zero, i.e. for a manipulated peer key).
* `curve25519.ed25519_public(seed)` returns the public key (32 bytes).
* `curve25519.ed25519_sign(seed, msg, pub)` returns the signature (64 bytes).
  `pub` is optional but saves a computation.
* `curve25519.ed25519_verify(pub, msg, sig)` returns `True` or `False`.
* Tested against RFC 7748, RFC 8032 and OpenSSL. Runs in constant time, on its
  own 1.5 KB stack in BRAM.

Measured on the hardware:

| Operation | Time |
|---|---|
| `ed25519_public` | 77 ms |
| `ed25519_sign` | 87 ms |
| `ed25519_verify` | 156 ms |
| `x25519` | 38 ms |

To measure it yourself (one line per measurement so typing time is not
counted; after the `def` line press Enter once on the empty line):

```python
import curve25519 as c, os, time
def T(n, f, *a): t = time.ticks_ms(); r = f(*a); print(n, time.ticks_diff(time.ticks_ms(), t), "ms"); return r

k = os.urandom(32); p = T("public", c.ed25519_public, k); s = T("sign", c.ed25519_sign, k, b"x" * 100, p); T("verify", c.ed25519_verify, p, b"x" * 100, s); T("x25519", c.x25519, k)
```

### Second console: `os.dupterm`

`os.dupterm(stream)` attaches a second console next to screen and keyboard.
Everything the REPL prints also goes to `stream.write()`; input is read from
`stream.readinto()` together with the keyboard. `os.dupterm(None)` detaches it.

* The stream must be **non-blocking** (socket: `setblocking(False)`; own class:
  `readinto` returns `None` when no data is available). Otherwise the keyboard
  stalls until data arrives.
* A custom stream is a subclass of `io.IOBase` with `readinto`, `write` and
  `ioctl` (this is how `sshd` encrypts and decrypts).
* If the stream raises an exception, it is detached (message "dupterm: ...
  deactivating") and the local console keeps working.
* Ctrl+C from the remote side works at the REPL and in `input()`. A running
  script only notices it when it next reads from the console.
* There is only one dupterm slot: this example and `sshd` cannot run at the
  same time.

```python
import net, socket, os
net.up()
srv = socket.socket()
srv.bind(("0.0.0.0", 2323)); srv.listen(1)
c, a = srv.accept()
c.setblocking(False)
os.dupterm(c)          # REPL now also via "nc <ip> 2323" (unencrypted!)
```

## Quick check

1. `net.up()` reaches an IP address.
2. After some traffic, `lan.status("stats")` shows few or no drops and CRC
   errors.
3. `requests.get("https://micropython.org/pi/v2/index.json")` returns status 200.
4. `mip.install("base64")` succeeds and `import base64` works afterwards.
5. `os.urandom(16)` gives different results after each power-on, and
   `random.seed` (64 bytes) exists in the MicroPython folder on the card.
6. The `os.dupterm` example: connect with `nc <ip> 2323` and type `1+1`; the
   answer appears both remotely and on screen.

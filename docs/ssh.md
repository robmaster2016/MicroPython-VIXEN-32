# SSH server

`sshd` is an SSH server written in MicroPython. It offers a shell (the REPL),
`scp` and `sftp`. It is frozen into the network builds (see
[Networking](network.md) and [Building](building.md)), so nothing needs to be
copied to the SD card.

```python
import sshd
sshd.start()          # returns at once; the server runs in the background
sshd.stop()
```

`start()` brings up the network if needed (like `net.up()`) and prints:

```
sshd: running on 192.168.1.50:22, files go to /sd/vixen-32/download
sshd: host key SHA256:...
```

Options: `sshd.start(port=22, user=None, password=None, home=None)`.
`user` (with an optional `password`; without one, only key login works)
overrides `sshd.txt` (see [Login](#login)); `home` replaces
the default file directory `/sd/vixen-32/download`.

## From the PC

```sh
ssh vixen@192.168.1.50                    # the MicroPython REPL
scp file.bin vixen@192.168.1.50:          # goes to /sd/vixen-32/download/
scp vixen@192.168.1.50:file.bin .         # fetches it from there
scp -r folder vixen@192.168.1.50:         # whole folders
scp file vixen@192.168.1.50:/sd/vixen-32/apps/micropython/   # absolute path
sftp vixen@192.168.1.50                   # interactive: ls, get, put, mkdir, rm ...
```

* Without a path, or with a relative path, files go to `/sd/vixen-32/download/`
  (created at startup). Paths starting with `/` are used as given.
* Both scp protocols work: the new one (SFTP, the default since OpenSSH 9) and
  the old one (`scp -O`).
* On the first connection ssh asks you to confirm the host key: its fingerprint
  must match the one `sshd.start()` printed on the screen.

## The shell

`ssh vixen@<ip>` gives you the REPL, the same one as on the screen: what you
type over SSH also appears there, and vice versa. Ctrl+C interrupts as usual.
**Ctrl+D ends the SSH session** (it does not soft-reset), except in paste mode
(Ctrl+E ... Ctrl+D). The ssh escape hatch: Enter, then `~.`.

## Login

All server files live in **`/sd/vixen-32/apps/micropython/ssh/`**:
`sshd.txt`, `authorized_keys` and `ssh_host_ed25519_key`. All server messages
are in English.

* **Password**: the first line of `ssh/sshd.txt`, in the form
  `user password`. Without the file, `vixen`/`vixen` applies and a warning is
  printed. An empty password means key login only.
* **Keys** (recommended): add the PC's public ed25519 key
  (`~/.ssh/id_ed25519.pub`, create it with `ssh-keygen -t ed25519`) as a line
  to `ssh/authorized_keys`. The user name is the one from `sshd.txt`. RSA keys
  are not supported.
* After 6 failed attempts the server disconnects; a connection that has not
  logged in after 2 minutes is closed.

## How it runs

`start()` attaches the server to the console via `os.dupterm`. The REPL polls
it whenever it waits for input, and the server does its work in these polls:
accepting connections, handling packets, transferring files. As a result:

* The server works while the REPL is waiting, which is almost always.
* While a script runs, SSH connections wait until it finishes. Short scripts
  are no problem; a script in an endless loop holds the server up. `input()`
  keeps serving it.
* `os.dupterm` has only one slot: the `dupterm` example from
  [Networking](network.md) and `sshd` cannot run at the same time.
* At most 2 connections at a time, one shell.

Autostart: put this in `boot/main.py` (in the MicroPython folder on the SD
card, `/vixen-32/apps/micropython`):

```python
import sshd
sshd.start()
```

## Technical details

* Key exchange `curve25519-sha256` (with strict kex), host key `ssh-ed25519`,
  ciphers `aes128-ctr`/`aes256-ctr`, MAC `hmac-sha2-256`. OpenSSH supports all
  of these without extra options.
* Speed on the real machine (LVGL + network build): scp ~45-62 KB/s,
  sftp ~36-62 KB/s. The handshake cryptography takes about 165 ms (see
  [curve25519 timings](network.md#module-curve25519)). AES and SHA-256 run on a
  stack in fast BRAM.
* The host key is stored as hex text in
  `/sd/vixen-32/apps/micropython/ssh/ssh_host_ed25519_key` and created on the
  first start. Deleting it creates a new key; ssh on the PC then shows a
  warning, and `ssh-keygen -R <ip>` removes the old entry. Create the host key
  after the random seed file exists (see
  [Random numbers](network.md#random-numbers)), i.e. not on the very first
  power-on.
* The SD card filesystem only supports reading and rewriting whole files:
  appending (`sftp` `reput`) does not work, and there are no file timestamps
  (sftp shows 1980).

## Quick check

1. `sshd.start()` prints the IP address and a fingerprint, and the fingerprint
   matches the one ssh shows on the first connection.
2. `ssh vixen@<ip>` reaches the `>>>` prompt and typing is responsive.
3. Copy a larger file (1-2 MB) there and back and compare:
   `scp file vixen@<ip>:`, `scp vixen@<ip>:file copy`, `cmp file copy`.
   Repeat with `scp -O`.
4. In the shell, `import network; network.LAN().status("stats")`: the third
   value is the number of dropped frames.
5. `sftp vixen@<ip>`, then `ls`, `mkdir test`, `put`, `get`, `rm`.

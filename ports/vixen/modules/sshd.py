# This file is part of MicroPython for the VIXEN-32.
#
# The MIT License (MIT)
#
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

# sshd.py -- SSH server for the VIXEN-32 (MicroPython, network build).
#
#   import sshd
#   sshd.start()          # returns at once; the server runs in the background
#   sshd.stop()
#
# What it offers:
#   ssh vixen@<ip>                    the MicroPython REPL (Ctrl-D ends the
#                                     session, Ctrl-C interrupts as usual)
#   scp file vixen@<ip>:              upload into /sd/vixen-32/download/
#   scp vixen@<ip>:file .             download from there
#   scp -r, scp -O (old protocol), sftp vixen@<ip>
# Paths without a leading "/" are relative to /sd/vixen-32/download/; absolute
# paths ("/sd/...") are used as given.
#
# All server files live in /sd/vixen-32/apps/micropython/ssh/:
#   sshd.txt              first line "user password" (default vixen/vixen,
#                         with a warning)
#   authorized_keys       ed25519 keys, lines as in ~/.ssh/authorized_keys
#   ssh_host_ed25519_key  the host key (created on first start; its
#                         fingerprint is printed so the first connection can
#                         be checked)
# Files found at the old places (apps/micropython/sshd.txt and
# ssh_host_ed25519_key, /sd/vixen-32/authorized_keys) are copied there once.
#
# Crypto: curve25519-sha256 key exchange, ssh-ed25519 host key (C module
# curve25519), aes128-ctr/aes256-ctr (cryptolib), hmac-sha2-256, strict kex.
#
# How it runs: start() installs the server as the os.dupterm stream. The REPL
# polls that stream while it waits for input, and each poll runs the server
# (accept, packets, file transfers). So the server works while the REPL is
# idle; a running script holds it up until it returns (connections then just
# wait). The SSH shell is the REPL itself, shared with the screen.

import socket
import struct
import hashlib
import binascii
import cryptolib
import os
import io
import time
import gc
import curve25519

HOME = "/sd/vixen-32/download"
SSH_DIR = "/sd/vixen-32/apps/micropython/ssh"
CONF = SSH_DIR + "/sshd.txt"
AUTH_KEYS = SSH_DIR + "/authorized_keys"
HOSTKEY = SSH_DIR + "/ssh_host_ed25519_key"
_OLD = ((CONF, "/sd/vixen-32/apps/micropython/sshd.txt"),
        (AUTH_KEYS, "/sd/vixen-32/authorized_keys"),
        (HOSTKEY, "/sd/vixen-32/apps/micropython/ssh_host_ed25519_key"))

VERSION = b"SSH-2.0-VIXEN32_sshd_1.0"
MAX_CONN = 2
# Sizes. The Ethernet core holds only 4 received frames; while Python is busy
# with a big block (AES + HMAC in one C call) nothing drains them, and the
# client's next requests are dropped -> TCP retransmit timeouts. Measured in
# vxemu (200 KB, sftp download): 16 KB blocks 23 s and 65 drops, 4 KB blocks
# with Nagle on 0.57 s and no drops. Incoming channel packets can stay larger.
WINDOW = 0x10000            # our receive window per channel
MAX_PKT = 0x4000            # largest channel data we accept per packet
IN_BUF = 0x10000            # receive buffer per connection (> largest packet)
OUT_HIGH = 0x1000           # stop producing when this much waits to be sent
SFTP_CHUNK = 0x1000         # largest SFTP read/write we announce
SCP_CHUNK = 0x1000          # file read size for scp downloads

_srv = None                 # the running server (dupterm stream)


# ---- encoding --------------------------------------------------------------

def _u32(n):
    return struct.pack(">I", n)


def _str(b):
    if isinstance(b, str):
        b = b.encode()
    return struct.pack(">I", len(b)) + b


def _mpint(b):
    i = 0
    while i < len(b) and b[i] == 0:
        i += 1
    b = b[i:]
    if b and b[0] & 0x80:
        b = b"\0" + b
    return _str(b)


class _Rd:
    def __init__(self, data, pos=0):
        self.d = data
        self.p = pos

    def byte(self):
        self.p += 1
        return self.d[self.p - 1]

    def bool(self):
        return self.byte() != 0

    def u32(self):
        self.p += 4
        return struct.unpack(">I", self.d[self.p - 4:self.p])[0]

    def u64(self):
        hi = self.u32()
        return (hi << 32) | self.u32()

    def str(self):
        n = self.u32()
        if self.p + n > len(self.d):
            raise ValueError("short packet")
        self.p += n
        return bytes(self.d[self.p - n:self.p])

    def names(self):
        s = self.str()
        return s.split(b",") if s else []


def _ct_eq(a, b):
    if len(a) != len(b):
        return False
    x = 0
    for i in range(len(a)):
        x |= a[i] ^ b[i]
    return x == 0


def _b64(b):
    return binascii.b2a_base64(b).strip().decode()


# ---- paths and files -------------------------------------------------------

def _norm(p):
    """Resolve a client path: relative ones live under HOME."""
    if isinstance(p, bytes):
        p = p.decode()
    if p in ("", ".", "~", "~/"):
        p = HOME
    elif p.startswith("~/"):
        p = HOME + p[1:]
    elif not p.startswith("/"):
        p = HOME + "/" + p
    out = []
    for part in p.split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            if out:
                out.pop()
        else:
            out.append(part)
    return "/" + "/".join(out)


def _stat(p):
    try:
        return os.stat(p)
    except OSError:
        return None


def _isdir(p):
    st = _stat(p)
    return st is not None and st[0] & 0x4000 != 0


def _mkdirs(p):
    cur = ""
    for part in p.strip("/").split("/"):
        cur += "/" + part
        if not _isdir(cur):
            try:
                os.mkdir(cur)
            except OSError:
                pass


def _join(d, name):
    return d.rstrip("/") + "/" + name if d != "/" else "/" + name


def _base(p):
    return p.rstrip("/").rsplit("/", 1)[-1]


def _log(*a):
    print("sshd:", *a)


# ---- the server ------------------------------------------------------------

class Server(io.IOBase):
    """The listening socket and all connections; also the dupterm stream."""

    def __init__(self, port, users, keys, seed):
        self.users = users          # {name: password}
        self.keys = keys            # set of ed25519 public key blobs
        self.seed = seed
        self.pub = curve25519.ed25519_public(seed)
        self.blob = _str("ssh-ed25519") + _str(self.pub)
        self.conns = []
        self.shell = None           # the channel the REPL talks to
        self.inq = bytearray()      # REPL input from the shell
        self.paste = False
        self.busy = False           # inside poll(): defer shell output
        self.outq = bytearray()     # REPL output collected meanwhile
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("0.0.0.0", port))
        s.listen(2)
        s.setblocking(False)
        self.sock = s

    def fingerprint(self):
        return "SHA256:" + _b64(hashlib.sha256(self.blob).digest()).rstrip("=")

    def close(self):
        for c in self.conns[:]:
            c.close()
        try:
            self.sock.close()
        except OSError:
            pass

    # dupterm side ----------------------------------------------------------

    def readinto(self, buf):
        if not self.inq:
            try:
                self.poll()
            except KeyboardInterrupt:
                self.inq.append(3)      # keep the local Ctrl-C for the REPL
            except Exception as e:
                _log("error", repr(e))
        if not self.inq:
            return None
        buf[0] = self.inq[0]
        self.inq = self.inq[1:]
        return 1

    def write(self, buf):
        ch = self.shell
        if ch is None or ch.closed:
            return len(buf)
        if self.busy:
            if len(self.outq) < 0x8000:
                self.outq.extend(buf)
            return len(buf)
        try:
            if ch.backlog() > 0x10000:      # client not reading: drop
                return len(buf)
            ch.send_data(bytes(buf))
            ch.conn.flush()
            # a script printing a lot: let the network drain, briefly
            t0 = time.ticks_ms()
            while ch.conn.pending() > 0x8000 and not ch.closed:
                time.sleep_ms(2)
                ch.conn.flush()
                if time.ticks_diff(time.ticks_ms(), t0) > 3000:
                    break
        except Exception:
            pass
        return len(buf)

    def ioctl(self, req, arg):
        if req == 3:                    # MP_STREAM_POLL
            r = 0
            if arg & 1 and self.inq:
                r |= 1
            if arg & 4:
                r |= 4
            return r
        return 0

    # network side ----------------------------------------------------------

    def poll(self):
        self.busy = True
        try:
            self._accept()
            for c in self.conns[:]:
                try:
                    c.poll()
                except Exception as e:
                    if not isinstance(e, OSError) or e.args[0] not in (104, 128, 113):
                        _log("connection", c.peer, "closed:", repr(e))
                    c.close()
        finally:
            self.busy = False
        if self.outq and self.shell and not self.shell.closed:
            q = self.outq
            self.outq = bytearray()
            self.shell.send_data(bytes(q))
            self.shell.conn.flush()

    def _accept(self):
        try:
            s, addr = self.sock.accept()
        except OSError:
            return
        if len(self.conns) >= MAX_CONN:
            s.close()
            return
        s.setblocking(False)
        # no TCP_NODELAY: Nagle keeps the segments full (fewer frames, fewer
        # ACK bursts back into the 4-frame receiver)
        self.conns.append(Conn(self, s, addr[0] if isinstance(addr, tuple) else addr))

    def shell_input(self, ch, data):
        for b in data:
            if b == 5:                  # Ctrl-E: paste mode, Ctrl-D ends it
                self.paste = True
            elif b == 3:
                self.paste = False
            elif b == 4:
                if self.paste:
                    self.paste = False
                else:
                    ch.finish(0)
                    return
            self.inq.append(b)


# ---- one SSH connection ----------------------------------------------------

KEX_ALGS = (b"curve25519-sha256", b"curve25519-sha256@libssh.org")
CIPHERS = {b"aes128-ctr": 16, b"aes256-ctr": 32}
MACS = (b"hmac-sha2-256",)


class Conn:
    def __init__(self, srv, sock, peer):
        self.srv = srv
        self.sock = sock
        self.peer = peer
        self.closed = False
        self.inb = bytearray(IN_BUF)  # fixed: recv straight into it
        self.inpos = 0              # consumed up to here
        self.inend = 0              # filled up to here
        self.outb = bytearray()
        self.outpos = 0             # sent up to here
        self.padpool = b""
        self.padpos = 0
        self.vc = None              # client version line
        self.rx = self.tx = None    # (aes, mac ipad, mac opad)
        self.rx_seq = self.tx_seq = 0
        self.rx_head = None         # decrypted first block of a packet
        self.session_id = None
        self.ic = self.is_ = None   # KEXINITs
        self.kex = False            # key exchange running
        self.kex_sent = False
        self.newkeys = None         # keys waiting for the client's NEWKEYS
        self.strict = False
        self.first = True           # still in the initial key exchange
        self.held = []              # payloads held back during a key exchange
        self.user = None
        self.auth_tries = 0
        self.chans = {}
        self.next_chan = 0
        self.t0 = time.ticks_ms()
        self.outb.extend(VERSION + b"\r\n")
        self.send_kexinit()

    def close(self):
        if self.closed:
            return
        self.closed = True
        for ch in list(self.chans.values()):
            ch.cleanup()
        try:
            self.flush()
        except Exception:
            pass
        try:
            self.sock.close()
        except OSError:
            pass
        if self in self.srv.conns:
            self.srv.conns.remove(self)
        if self.user:
            _log("logout", self.user, "from", self.peer)

    def disconnect(self, reason, text):
        try:
            self.send(bytes([1]) + _u32(reason) + _str(text) + _str(""))
        except Exception:
            pass
        raise OSError(128)

    # byte level ------------------------------------------------------------

    def pending(self):
        return len(self.outb) - self.outpos

    # Buffers are consumed by moving an offset, not by slicing off the front:
    # on the VIXEN every copy of a few KB costs milliseconds.
    def flush(self):
        mv = memoryview(self.outb)
        while self.outpos < len(self.outb):
            try:
                n = self.sock.send(mv[self.outpos:self.outpos + 4096])
            except OSError as e:
                if e.args[0] == 11:     # EAGAIN
                    break
                raise
            if not n:
                break
            self.outpos += n
        mv = None
        if self.outpos == len(self.outb):
            self.outb = bytearray()
            self.outpos = 0
        elif self.outpos > 0x4000:
            self.outb = self.outb[self.outpos:]
            self.outpos = 0

    def _avail(self):
        return self.inend - self.inpos

    def _take(self, n):
        b = bytes(memoryview(self.inb)[self.inpos:self.inpos + n])
        self.inpos += n
        return b

    def _compact(self):
        # move the unprocessed rest to the front (usually a few bytes)
        n = self.inend - self.inpos
        if n:
            self.inb[:n] = memoryview(self.inb)[self.inpos:self.inend]
        self.inpos = 0
        self.inend = n

    def poll(self):
        # bytearray.extend() reallocates (and may copy) the whole buffer on
        # every call, so the data is read into a fixed buffer instead
        if self.inpos == self.inend:
            self.inpos = self.inend = 0
        elif self.inpos and (self.inpos > IN_BUF // 2 or IN_BUF - self.inend < 0x9000):
            self._compact()             # room for the largest packet (35000)
        mv = memoryview(self.inb)
        while self.inend < IN_BUF:
            n = self.sock.readinto(mv[self.inend:])
            if n is None:               # EAGAIN
                break
            if not n:
                raise OSError(104)      # closed by the peer
            self.inend += n
        mv = None
        if self.vc is None:
            self._version()
        if self.vc is not None:
            while not self.closed:
                p = self._packet()
                if p is None:
                    break
                self._dispatch(p)
        if not self.closed:
            for ch in list(self.chans.values()):
                ch.run()
            self.flush()
            if self.user is None and time.ticks_diff(time.ticks_ms(), self.t0) > 120000:
                self.disconnect(2, "login timeout")

    def _version(self):
        buf = self.inb[self.inpos:self.inend]
        i = buf.find(b"\n")
        if i < 0:
            if len(buf) > 2048:
                raise OSError(128)
            return
        line = bytes(buf[:i]).rstrip(b"\r")
        self._take(i + 1)
        if not line.startswith(b"SSH-"):
            return self._version()      # banner lines before the version
        if not line.startswith(b"SSH-2.0-"):
            raise OSError(128)
        self.vc = line

    # packet level ----------------------------------------------------------

    def _packet(self):
        bs = 16 if self.rx else 8
        if self.rx_head is None:
            if self._avail() < bs:
                return None
            h = self._take(bs)
            if self.rx:
                h = self.rx[0].encrypt(h)
            self.rx_head = h
        h = self.rx_head
        plen = struct.unpack(">I", h[:4])[0]
        if plen < 12 or plen > 35000 or (plen + 4) % bs:
            raise OSError(128)
        maclen = 32 if self.rx else 0
        rest = plen + 4 - bs
        if self._avail() < rest + maclen:
            return None
        # decrypt straight from the receive buffer into the packet
        pkt = bytearray(plen + 4)
        pkt[:bs] = h
        i = self.inpos
        src = memoryview(self.inb)[i:i + rest + maclen]
        if self.rx:
            self.rx[0].encrypt(src[:rest], memoryview(pkt)[bs:])
            if not _ct_eq(src[rest:], _hmac(self.rx, self.rx_seq, pkt)):
                self.disconnect(5, "bad MAC")
        else:
            pkt[bs:] = src
        src = None
        self.inpos = i + rest + maclen
        self._take(0)                   # compacts the buffer now and then
        self.rx_head = None
        self.rx_seq = (self.rx_seq + 1) & 0xFFFFFFFF
        pad = pkt[4]
        return bytes(memoryview(pkt)[5:4 + plen - pad])

    def send(self, payload):
        t = payload[0]
        if self.kex and not (1 <= t <= 4 or 20 <= t <= 49):
            self.held.append(payload)
            return
        bs = 16 if self.tx else 8
        pad = bs - (5 + len(payload)) % bs
        if pad < 4:
            pad += bs
        n = len(payload)
        pkt = bytearray(5 + n + pad)
        struct.pack_into(">IB", pkt, 0, 1 + n + pad, pad)
        pkt[5:5 + n] = payload
        pkt[5 + n:] = self._pad(pad)
        if self.tx:
            mac = _hmac(self.tx, self.tx_seq, pkt)
            self.tx[0].encrypt(pkt, pkt)       # in place
            self.outb.extend(pkt)
            self.outb.extend(mac)
        else:
            self.outb.extend(pkt)
        self.tx_seq = (self.tx_seq + 1) & 0xFFFFFFFF

    def _pad(self, n):
        # Random padding from a pool: os.urandom costs ~4 ms per call on the
        # VIXEN, i.e. per packet. The padding is encrypted anyway (CTR).
        if self.padpos + n > len(self.padpool):
            self.padpool = os.urandom(256)
            self.padpos = 0
        self.padpos += n
        return self.padpool[self.padpos - n:self.padpos]

    # key exchange ----------------------------------------------------------

    def send_kexinit(self):
        kex = b",".join(KEX_ALGS)
        if self.first:
            kex += b",kex-strict-s-v00@openssh.com"
        ciph = b",".join(CIPHERS)
        p = (bytes([20]) + os.urandom(16) + _str(kex) + _str("ssh-ed25519")
             + _str(ciph) + _str(ciph) + _str(b",".join(MACS)) * 2
             + _str("none") * 2 + _str("") * 2 + b"\0" + _u32(0))
        self.is_ = p
        self.kex = True
        self.kex_sent = True
        self.send(p)

    def _kexinit(self, p):
        if not self.kex_sent:
            self.send_kexinit()
        self.ic = p
        r = _Rd(p, 17)
        kex = r.names()
        hk = r.names()
        c_cs, c_sc = r.names(), r.names()
        m_cs, m_sc = r.names(), r.names()
        z_cs, z_sc = r.names(), r.names()

        def pick(client, ours, what):
            for a in client:
                if a in ours:
                    return a
            self.disconnect(3, "no common " + what)

        if self.first and b"kex-strict-c-v00@openssh.com" in kex:
            self.strict = True
        pick(kex, KEX_ALGS, "kex")
        pick(hk, (b"ssh-ed25519",), "host key type")
        self.alg = (pick(c_cs, CIPHERS, "cipher"), pick(c_sc, CIPHERS, "cipher"))
        pick(m_cs, MACS, "mac")
        pick(m_sc, MACS, "mac")
        pick(z_cs, (b"none",), "compression")
        pick(z_sc, (b"none",), "compression")

    def _ecdh(self, p):
        qc = _Rd(p, 1).str()
        if len(qc) != 32:
            self.disconnect(3, "bad key")
        e = os.urandom(32)
        qs = curve25519.x25519(e)
        k = _mpint(curve25519.x25519(e, qc))
        h = hashlib.sha256()
        for part in (self.vc, VERSION, self.ic, self.is_, self.srv.blob, qc, qs):
            h.update(_str(part))
        h.update(k)
        h = h.digest()
        if self.session_id is None:
            self.session_id = h
        sig = curve25519.ed25519_sign(self.srv.seed, h, self.srv.pub)
        self.send(bytes([31]) + _str(self.srv.blob) + _str(qs)
                  + _str(_str("ssh-ed25519") + _str(sig)))
        self.send(bytes([21]))          # NEWKEYS

        def key(c, n):
            return hashlib.sha256(k + h + c + self.session_id).digest()[:n]

        def side(iv, ek, mk, alg):
            aes = cryptolib.aes(key(ek, CIPHERS[alg]), cryptolib.MODE_CTR, key(iv, 16))
            mk = key(mk, 32) + bytes(32)
            return (aes, bytes(b ^ 0x36 for b in mk), bytes(b ^ 0x5C for b in mk))

        self.tx = side(b"B", b"D", b"F", self.alg[1])
        if self.strict:
            self.tx_seq = 0
        self.newkeys = side(b"A", b"C", b"E", self.alg[0])
        self.kex = False
        self.kex_sent = False
        held, self.held = self.held, []
        for q in held:
            self.send(q)

    # messages --------------------------------------------------------------

    def _dispatch(self, p):
        t = p[0]
        if self.first:
            # the first message must be KEXINIT; with strict kex nothing but
            # the key exchange may follow until the first NEWKEYS
            if self.ic is None:
                if t not in (20, 2, 4):
                    self.disconnect(3, "expected KEXINIT")
            elif self.strict and t not in (30, 21):
                self.disconnect(3, "unexpected message during key exchange")
        if t == 20:
            self._kexinit(p)
        elif t == 30:
            if self.ic is None:
                self.disconnect(3, "ECDH before KEXINIT")
            self._ecdh(p)
        elif t == 21:
            if self.newkeys is None:
                self.disconnect(3, "NEWKEYS before ECDH")
            self.rx = self.newkeys
            self.newkeys = None
            if self.strict:
                self.rx_seq = 0
            self.first = False
        elif t == 1:
            raise OSError(128)
        elif t in (2, 4):               # IGNORE, DEBUG
            pass
        elif t == 3:                    # UNIMPLEMENTED
            pass
        elif self.rx is None:
            self.disconnect(2, "not encrypted")
        elif t == 5:
            name = _Rd(p, 1).str()
            if name != b"ssh-userauth":
                self.disconnect(7, "service not available")
            self.send(bytes([6]) + _str(name))
        elif t == 50:
            self._userauth(p)
        elif self.user is None:
            self.disconnect(2, "not authenticated")
        elif t == 80:                   # GLOBAL_REQUEST
            r = _Rd(p, 1)
            r.str()
            if r.bool():
                self.send(bytes([82]))
        elif t == 90:
            self._open(p)
        elif 91 <= t <= 100:
            r = _Rd(p, 1)
            ch = self.chans.get(r.u32())
            if ch is not None:
                ch.message(t, r)
        else:
            self.send(bytes([3]) + _u32((self.rx_seq - 1) & 0xFFFFFFFF))

    def _userauth(self, p):
        if self.user is not None:
            return
        r = _Rd(p, 1)
        user = r.str().decode()
        service = r.str()
        method = r.str()
        ok = False
        if service != b"ssh-connection":
            self.disconnect(7, "service not available")
        if method == b"password":
            r.bool()
            pw = r.str().decode()
            ok = user in self.srv.users and self.srv.users[user] != "" and _ct_eq(
                self.srv.users[user].encode(), pw.encode())
        elif method == b"publickey":
            has_sig = r.bool()
            alg = r.str()
            blob = r.str()
            if alg == b"ssh-ed25519" and blob in self.srv.keys and user in self.srv.users:
                if not has_sig:
                    self.send(bytes([60]) + _str(alg) + _str(blob))
                    return
                sr = _Rd(r.str())
                sr.str()
                sig = sr.str()
                br = _Rd(blob)
                br.str()
                pk = br.str()
                data = (_str(self.session_id) + bytes([50]) + _str(user) + _str(service)
                        + _str("publickey") + b"\1" + _str(alg) + _str(blob))
                ok = len(sig) == 64 and len(pk) == 32 and curve25519.ed25519_verify(pk, data, sig)
        if ok:
            self.user = user
            self.send(bytes([52]))
            _log("login", user, "from", self.peer, "(" + method.decode() + ")")
            return
        if method != b"none":
            self.auth_tries += 1
            if self.auth_tries >= 6:
                self.disconnect(14, "too many authentication failures")
        methods = "publickey,password" if self.srv.users.get(user, "") != "" else "publickey"
        self.send(bytes([51]) + _str(methods) + b"\0")

    def _open(self, p):
        r = _Rd(p, 1)
        kind = r.str()
        peer_id, win, mx = r.u32(), r.u32(), r.u32()
        if kind != b"session" or len(self.chans) >= 4:
            self.send(bytes([92]) + _u32(peer_id) + _u32(1) + _str("only sessions") + _str(""))
            return
        cid = self.next_chan
        self.next_chan += 1
        self.chans[cid] = Channel(self, cid, peer_id, win, mx)
        self.send(bytes([91]) + _u32(peer_id) + _u32(cid) + _u32(WINDOW) + _u32(MAX_PKT))


def _hmac(k, seq, data):
    h = hashlib.sha256(k[1])
    h.update(_u32(seq))
    h.update(data)
    o = hashlib.sha256(k[2])
    o.update(h.digest())
    return o.digest()


# ---- channels --------------------------------------------------------------

class Channel:
    def __init__(self, conn, cid, peer_id, win, mx):
        self.conn = conn
        self.id = cid
        self.peer_id = peer_id
        self.peer_win = win
        self.peer_max = min(mx, MAX_PKT)
        self.win = WINDOW
        self.rx = bytearray()
        self.rxpos = 0
        self.out = bytearray()      # data waiting for window
        self.outpos = 0
        self.eof = False
        self.closed = False
        self.sent_close = False
        self.proc = None            # generator: scp / sftp
        self.is_shell = False

    # sending ---------------------------------------------------------------

    def send_data(self, data):
        if self.sent_close:
            return
        self.out.extend(data)
        self._push()

    def _push(self):
        while self.outpos < len(self.out) and self.peer_win > 0:
            n = min(len(self.out) - self.outpos, self.peer_win, self.peer_max)
            p = bytearray(9 + n)        # CHANNEL_DATA: id, string
            struct.pack_into(">BII", p, 0, 94, self.peer_id, n)
            p[9:] = memoryview(self.out)[self.outpos:self.outpos + n]
            self.conn.send(p)
            self.outpos += n
            self.peer_win -= n
        if self.outpos == len(self.out):
            self.out = bytearray()
            self.outpos = 0
        elif self.outpos > 0x4000:
            self.out = self.out[self.outpos:]
            self.outpos = 0

    def backlog(self):
        return len(self.out) - self.outpos + self.conn.pending()

    def finish(self, status):
        if self.sent_close:
            return
        self._push()
        c = self.conn
        c.send(bytes([98]) + _u32(self.peer_id) + _str("exit-status") + b"\0" + _u32(status))
        c.send(bytes([96]) + _u32(self.peer_id))
        c.send(bytes([97]) + _u32(self.peer_id))
        self.sent_close = True
        self.cleanup()

    def cleanup(self):
        srv = self.conn.srv
        if srv.shell is self:
            srv.shell = None
            srv.paste = False
        if self.proc is not None:
            try:
                self.proc.close()
            except Exception:
                pass
            self.proc = None

    # receiving -------------------------------------------------------------

    def avail(self):
        return len(self.rx) - self.rxpos

    def take(self, n):
        b = bytes(self.rx[self.rxpos:self.rxpos + n])
        self.rxpos += n
        if self.rxpos > 0x4000 or self.rxpos == len(self.rx):
            self.rx = self.rx[self.rxpos:]
            self.rxpos = 0
        return b

    def message(self, t, r):
        c = self.conn
        if t == 93:                     # WINDOW_ADJUST
            self.peer_win += r.u32()
            self._push()
        elif t == 94:                   # DATA
            d = r.str()
            self.win -= len(d)
            if self.win < 0:
                c.disconnect(2, "window exceeded")
            if self.is_shell:
                c.srv.shell_input(self, d)
                self._grant(len(d))
            else:
                self.rx.extend(d)
        elif t == 95:                   # EXTENDED_DATA: ignored
            r.u32()
            self._grant(len(r.str()))
        elif t == 96:                   # EOF
            self.eof = True
        elif t == 97:                   # CLOSE
            if not self.sent_close:
                c.send(bytes([97]) + _u32(self.peer_id))
                self.sent_close = True
            self.closed = True
            self.cleanup()
            c.chans.pop(self.id, None)
        elif t == 98:
            self._request(r)

    def _grant(self, n):
        # refill the window once half of it is used and the data is processed
        used = WINDOW - self.win
        if used >= WINDOW // 2 and self.avail() < WINDOW // 2:
            self.conn.send(bytes([93]) + _u32(self.peer_id) + _u32(used))
            self.win += used

    def _request(self, r):
        c = self.conn
        kind = r.str()
        want = r.bool()
        ok = True
        if kind in (b"pty-req", b"env", b"window-change", b"signal"):
            pass
        elif kind == b"shell" and self.proc is None and not self.is_shell:
            srv = c.srv
            if srv.shell is not None and not srv.shell.closed:
                ok = False
            else:
                srv.shell = self
                self.is_shell = True
                srv.inq = bytearray()
                self.send_data(b"VIXEN-32 MicroPython over SSH. Ctrl-D ends the session.\r\n")
                srv.inq.extend(b"\r")   # a fresh prompt
        elif kind == b"exec" and self.proc is None and not self.is_shell:
            cmd = r.str().decode()
            self.proc = _exec(self, cmd)
        elif kind == b"subsystem" and self.proc is None and not self.is_shell:
            if r.str() == b"sftp":
                self.proc = _sftp(self)
            else:
                ok = False
        else:
            ok = False
        if want:
            c.send(bytes([99 if ok else 100]) + _u32(self.peer_id))

    def run(self):
        if self.proc is None or self.sent_close:
            return
        try:
            next(self.proc)
        except StopIteration as e:
            self.proc = None
            self.finish(e.value or 0)
        self._grant(0)

    # helpers for the generators ---------------------------------------------

    def need(self, n):
        """yield until n bytes are there; False at EOF"""
        while self.avail() < n:
            if self.eof:
                return False
            yield
        return True

    def drain(self):
        """yield while too much output waits"""
        while self.backlog() > OUT_HIGH:
            yield


# ---- exec: scp -------------------------------------------------------------

def _split(cmd):
    out, cur, q, i, have = [], "", None, 0, False
    while i < len(cmd):
        ch = cmd[i]
        if q:
            if ch == q:
                q = None
            elif ch == "\\" and q == '"' and i + 1 < len(cmd):
                i += 1
                cur += cmd[i]
            else:
                cur += ch
        elif ch in "'\"":
            q = ch
            have = True
        elif ch == "\\" and i + 1 < len(cmd):
            i += 1
            cur += cmd[i]
            have = True
        elif ch in " \t":
            if cur or have:
                out.append(cur)
            cur, have = "", False
        else:
            cur += ch
        i += 1
    if cur or have:
        out.append(cur)
    return out


def _exec(ch, cmd):
    args = _split(cmd)
    if not args or args[0] != "scp":
        ch.send_data(b"sshd: only the shell, scp and sftp are available\r\n")
        return 127
    flags, paths = "", []
    for a in args[1:]:
        if a.startswith("-") and not paths and a != "--":
            flags += a[1:]
        elif a != "--":
            paths.append(a)
    if "t" in flags:
        r = yield from _scp_sink(ch, _norm(paths[0] if paths else ""), "d" in flags)
    elif "f" in flags:
        r = yield from _scp_source(ch, [_norm(p) for p in paths], "r" in flags)
    else:
        r = 1
    return r


def _readline(ch):
    while True:
        buf = ch.rx[ch.rxpos:]
        i = buf.find(b"\n")
        if i >= 0:
            return ch.take(i + 1)[:-1]
        if ch.eof:
            return None
        yield


def _scp_sink(ch, target, want_dir):
    if want_dir and not _isdir(target):
        ch.send_data(b"\x02scp: " + target.encode() + b": not a directory\n")
        return 1
    dirs = [target]
    ch.send_data(b"\0")
    errors = 0
    while True:
        line = yield from _readline(ch)
        if line is None:
            return 1 if errors else 0
        if not line:
            continue
        cmd = line[0:1]
        if cmd in (b"\1", b"\2"):
            errors += 1
            continue
        if cmd == b"T":
            ch.send_data(b"\0")
            continue
        if cmd == b"E":
            if len(dirs) > 1:
                dirs.pop()
            ch.send_data(b"\0")
            continue
        try:
            mode, size, name = line[1:].decode().split(" ", 2)
            size = int(size)
        except ValueError:
            ch.send_data(b"\x02scp: bad command\n")
            return 1
        if "/" in name or name in ("..", "."):
            ch.send_data(b"\x02scp: bad file name\n")
            return 1
        cur = dirs[-1]
        if cmd == b"D":
            p = _join(cur, name) if _isdir(cur) else cur
            if not _isdir(p):
                try:
                    os.mkdir(p)
                except OSError:
                    ch.send_data(b"\x01scp: " + p.encode() + b": cannot create directory\n")
                    return 1
            dirs.append(p)
            ch.send_data(b"\0")
            continue
        if cmd != b"C":
            ch.send_data(b"\x02scp: bad command\n")
            return 1
        p = _join(cur, name) if _isdir(cur) else cur
        try:
            f = open(p, "wb")
        except OSError as e:
            ch.send_data(b"\x01scp: " + p.encode() + b": " + str(e).encode() + b"\n")
            errors += 1
            # the client sends the data anyway: swallow it
            f = None
        if f is not None:
            ch.send_data(b"\0")
        left = size
        try:
            while left:
                if not ch.avail():
                    if ch.eof:
                        return 1
                    yield
                    continue
                d = ch.take(min(left, ch.avail(), 0x4000))
                left -= len(d)
                if f is not None:
                    f.write(d)
            ok = yield from ch.need(1)
            if not ok:
                return 1
            ch.take(1)
        finally:
            if f is not None:
                f.close()
        if f is not None:
            _log("scp received", p, size, "bytes")
            ch.send_data(b"\0")


def _wait_ack(ch):
    ok = yield from ch.need(1)
    if not ok:
        return False
    b = ch.take(1)
    if b == b"\0":
        return True
    yield from _readline(ch)        # the error text
    return False


def _scp_source(ch, paths, recursive):
    ok = yield from _wait_ack(ch)
    if not ok:
        return 1
    errors = 0
    for p in paths:
        r = yield from _scp_send(ch, p, recursive)
        if r is None:
            return 1
        errors += r
    return 1 if errors else 0


def _scp_send(ch, p, recursive):
    """0 = ok, 1 = a soft error, None = the client gave up"""
    st = _stat(p)
    if st is None:
        ch.send_data(b"\x01scp: " + p.encode() + b": No such file or directory\n")
        return 1
    name = _base(p).encode() or b"root"
    if st[0] & 0x4000:
        if not recursive:
            ch.send_data(b"\x01scp: " + p.encode() + b": not a regular file\n")
            return 1
        ch.send_data(b"D0755 0 " + name + b"\n")
        ok = yield from _wait_ack(ch)
        if not ok:
            return None
        err = 0
        for e in sorted(x[0] for x in os.ilistdir(p)):
            r = yield from _scp_send(ch, _join(p, e), True)
            if r is None:
                return None
            err += r
        ch.send_data(b"E\n")
        ok = yield from _wait_ack(ch)
        return err if ok else None
    size = st[6]
    try:
        f = open(p, "rb")
    except OSError:
        ch.send_data(b"\x01scp: " + p.encode() + b": cannot open\n")
        return 1
    try:
        ch.send_data(("C0644 %d " % size).encode() + name + b"\n")
        ok = yield from _wait_ack(ch)
        if not ok:
            return None
        left = size
        while left > 0:
            yield from ch.drain()
            d = f.read(min(left, SCP_CHUNK))
            if not d:
                break
            ch.send_data(d)
            left -= len(d)
        if left:
            ch.send_data(bytes(left))   # file shrank: keep the protocol in step
    finally:
        f.close()
    ch.send_data(b"\0")
    ok = yield from _wait_ack(ch)
    if ok:
        _log("scp sent", p, size, "bytes")
    return 0 if ok else None


# ---- subsystem: sftp (version 3) -------------------------------------------

def _attrs(st):
    if st is None:
        return _u32(0)
    perm = 0o40755 if st[0] & 0x4000 else 0o100644
    return _u32(1 | 4) + struct.pack(">II", 0, st[6]) + _u32(perm)


def _longname(name, st):
    d = st[0] & 0x4000
    return "%s    1 vixen    vixen    %8d Jan  1  1980 %s" % (
        "drwxr-xr-x" if d else "-rw-r--r--", 0 if d else st[6], name)


def _skip_attrs(r):
    fl = r.u32()
    if fl & 1:
        r.u64()
    if fl & 2:
        r.u32()
        r.u32()
    if fl & 4:
        r.u32()
    if fl & 8:
        r.u32()
        r.u32()
    if fl & 0x80000000:
        for _ in range(r.u32()):
            r.str()
            r.str()


class _FH:
    def __init__(self, path, f, write):
        self.path = path
        self.f = f
        self.write = write
        self.pos = 0
        self.ents = None


def _sftp(ch):
    handles = {}
    nexth = [0]

    def reply(t, rid, body=b""):
        ch.send_data(_u32(5 + len(body)) + bytes([t]) + _u32(rid) + body)

    def status(rid, code, msg=""):
        reply(101, rid, _u32(code) + _str(msg) + _str(""))

    def err(rid, e):
        code = 2 if isinstance(e, OSError) and e.args and e.args[0] == 2 else 4
        status(rid, code, str(e))

    def handle(rid, obj):
        h = str(nexth[0]).encode()
        nexth[0] += 1
        handles[h] = obj
        reply(102, rid, _str(h))

    try:
        while True:
            ok = yield from ch.need(4)
            if not ok:
                return 0
            n = struct.unpack(">I", ch.rx[ch.rxpos:ch.rxpos + 4])[0]
            if n > 0x8000 + 1024:
                return 1
            ok = yield from ch.need(4 + n)
            if not ok:
                return 0
            yield from ch.drain()
            pkt = ch.take(4 + n)
            t = pkt[4]
            r = _Rd(pkt, 5)
            if t == 1:                  # INIT
                ch.send_data(_u32(5 + 4 + 18 + 5) + b"\x02" + _u32(3)
                             + _str("limits@openssh.com") + _str("1"))
                continue
            rid = r.u32()
            try:
                if t == 16:             # REALPATH
                    p = _norm(r.str())
                    reply(104, rid, _u32(1) + _str(p) + _str(p) + _u32(0))
                elif t in (17, 7):      # STAT, LSTAT
                    st = _stat(_norm(r.str()))
                    if st is None:
                        status(rid, 2, "No such file")
                    else:
                        reply(105, rid, _attrs(st))
                elif t == 8:            # FSTAT
                    fh = handles.get(r.str())
                    st = _stat(fh.path) if fh else None
                    if fh and fh.write:
                        st = (0x8000, 0, 0, 0, 0, 0, fh.pos, 0, 0, 0)
                    if st is None:
                        status(rid, 4, "bad handle")
                    else:
                        reply(105, rid, _attrs(st))
                elif t == 3:            # OPEN
                    p = _norm(r.str())
                    pf = r.u32()
                    if pf & 2:          # WRITE (the card knows 'w' only: truncates)
                        if pf & 4:
                            status(rid, 8, "append not supported")
                            continue
                        if pf & 0x20 and _stat(p) is not None:
                            status(rid, 4, "file exists")
                            continue
                        handle(rid, _FH(p, open(p, "wb"), True))
                    else:
                        handle(rid, _FH(p, open(p, "rb"), False))
                elif t == 4:            # CLOSE
                    fh = handles.pop(r.str(), None)
                    if fh and fh.f:
                        fh.f.close()
                        if fh.write:
                            _log("sftp received", fh.path, fh.pos, "bytes")
                    status(rid, 0)
                elif t == 5:            # READ
                    fh = handles.get(r.str())
                    off, ln = r.u64(), r.u32()
                    if not fh or fh.write or fh.f is None:
                        status(rid, 4, "bad handle")
                        continue
                    if off != fh.pos:
                        fh.f.seek(off)
                        fh.pos = off
                    d = fh.f.read(min(ln, SFTP_CHUNK))
                    if not d:
                        status(rid, 1, "EOF")
                    else:
                        fh.pos += len(d)
                        reply(103, rid, _str(d))
                elif t == 6:            # WRITE
                    fh = handles.get(r.str())
                    off = r.u64()
                    d = r.str()
                    if not fh or not fh.write:
                        status(rid, 4, "bad handle")
                    elif off != fh.pos:
                        status(rid, 8, "only sequential writes")
                    else:
                        fh.f.write(d)
                        fh.pos += len(d)
                        status(rid, 0)
                elif t == 11:           # OPENDIR
                    p = _norm(r.str())
                    if not _isdir(p):
                        status(rid, 2, "No such directory")
                        continue
                    fh = _FH(p, None, False)
                    fh.ents = [x for x in os.ilistdir(p)]
                    handle(rid, fh)
                elif t == 12:           # READDIR
                    fh = handles.get(r.str())
                    if not fh or fh.ents is None:
                        status(rid, 4, "bad handle")
                    elif not fh.ents:
                        status(rid, 1, "EOF")
                    else:
                        part, fh.ents = fh.ents[:32], fh.ents[32:]
                        body = bytearray(_u32(len(part)))
                        for e in part:
                            st = (e[1], 0, 0, 0, 0, 0, e[3] if len(e) > 3 else 0, 0, 0, 0)
                            body += _str(e[0]) + _str(_longname(e[0], st)) + _attrs(st)
                        reply(104, rid, body)
                elif t == 13:           # REMOVE
                    os.remove(_norm(r.str()))
                    status(rid, 0)
                elif t == 14:           # MKDIR
                    os.mkdir(_norm(r.str()))
                    status(rid, 0)
                elif t == 15:           # RMDIR
                    os.rmdir(_norm(r.str()))
                    status(rid, 0)
                elif t == 18:           # RENAME
                    a = _norm(r.str())
                    os.rename(a, _norm(r.str()))
                    status(rid, 0)
                elif t in (9, 10):      # SETSTAT, FSETSTAT: nothing to set
                    status(rid, 0)
                elif t == 200:          # EXTENDED
                    if r.str() == b"limits@openssh.com":
                        reply(201, rid, struct.pack(">IIIIIIII", 0, SFTP_CHUNK + 1024,
                                                    0, SFTP_CHUNK, 0, SFTP_CHUNK, 0, 8))
                    else:
                        status(rid, 8, "unsupported")
                else:
                    status(rid, 8, "unsupported")
            except OSError as e:
                err(rid, e)
    finally:
        for fh in handles.values():
            if fh.f:
                try:
                    fh.f.close()
                except OSError:
                    pass


# ---- setup -----------------------------------------------------------------

def _migrate():
    # copy files from the places older versions used; the originals stay
    _mkdirs(SSH_DIR)
    for new, old in _OLD:
        if _stat(new) is not None or _stat(old) is None:
            continue
        try:
            with open(old, "rb") as f:
                data = f.read()
            with open(new, "wb") as f:
                f.write(data)
            _log("copied", old, "->", new)
        except OSError as e:
            _log("cannot copy", old, "->", new, repr(e))


def _load_users(user, password):
    if user is not None:
        return {user: password or ""}
    try:
        with open(CONF) as f:
            line = f.readline().strip()
        sp = line.find(" ")
        if sp > 0:
            return {line[:sp]: line[sp + 1:]}
    except OSError:
        pass
    _log("no", CONF, "-- login is vixen/vixen (please change!)")
    return {"vixen": "vixen"}


def _load_keys():
    keys = set()
    try:
        with open(AUTH_KEYS) as f:
            for line in f:
                parts = line.split()
                for i, w in enumerate(parts[:-1]):
                    if w == "ssh-ed25519":
                        try:
                            keys.add(binascii.a2b_base64(parts[i + 1]))
                        except ValueError:
                            pass
                        break
    except OSError:
        pass
    return keys


def _host_seed():
    try:
        with open(HOSTKEY) as f:
            s = binascii.unhexlify(f.read().strip())
        if len(s) == 32:
            return s
    except (OSError, ValueError):
        pass
    s = os.urandom(32)
    try:
        with open(HOSTKEY, "w") as f:
            f.write(binascii.hexlify(s).decode() + "\n")
        _log("new host key in", HOSTKEY)
    except OSError:
        _log("cannot save the host key, it is valid until the next restart")
    return s


def _ip():
    try:
        import network
        lan = network.LAN()
        if not lan.isconnected():
            import net
            net.up()
        return lan.ifconfig()[0]
    except Exception as e:
        _log("network:", repr(e))
        return "?"


def start(port=22, user=None, password=None, home=None):
    """Start the server in the background (as the os.dupterm stream)."""
    global _srv, HOME
    stop()
    if home:
        HOME = home.rstrip("/") or "/"
    _mkdirs(HOME)
    _migrate()
    gc.collect()
    ip = _ip()
    srv = Server(port, _load_users(user, password), _load_keys(), _host_seed())
    _srv = srv
    os.dupterm(srv)
    _log("running on %s:%d, files go to %s" % (ip, port, HOME))
    _log("host key", srv.fingerprint())
    if srv.keys:
        _log(len(srv.keys), "key(s) from", AUTH_KEYS)
    return srv


def stop():
    global _srv
    if _srv is not None:
        try:
            os.dupterm(None)
        except Exception:
            pass
        _srv.close()
        _srv = None

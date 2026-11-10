# HTTPS with requests and package installs with mip (into a RAM filesystem).
# Needs the internet.
import network, time, sys, vfs, io, gc
lan = network.LAN(); lan.active(True)
t = time.ticks_ms()
while not lan.isconnected():
    time.sleep_ms(50)
print("ip", lan.ifconfig()[0], "dhcp ms", time.ticks_diff(time.ticks_ms(), t))

class RamFile(io.IOBase):
    def __init__(self, fs, path, mode):
        self.fs, self.path, self.mode = fs, path, mode
        self.buf = bytearray() if "w" in mode else bytearray(fs.files[path])
        self.pos = 0
    def write(self, b):
        self.buf.extend(b); return len(b)
    def readinto(self, b):
        n = min(len(b), len(self.buf) - self.pos)
        b[:n] = self.buf[self.pos:self.pos + n]; self.pos += n; return n
    def read(self, n=-1):
        if n < 0: n = len(self.buf) - self.pos
        r = bytes(self.buf[self.pos:self.pos + n]); self.pos += len(r); return r
    def close(self):
        if "w" in self.mode: self.fs.files[self.path] = bytes(self.buf)
    def __enter__(self): return self
    def __exit__(self, *a): self.close()

class RamFS:
    def __init__(self): self.files = {}; self.dirs = {""}
    def mount(self, ro, mkfs): pass
    def umount(self): pass
    def _p(self, p): return p.strip("/")
    def stat(self, p):
        p = self._p(p)
        if p in self.dirs: return (0x4000, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        if p in self.files: return (0x8000, 0, 0, 0, 0, 0, len(self.files[p]), 0, 0, 0)
        raise OSError(2)
    def mkdir(self, p):
        p = self._p(p)
        if p in self.dirs: raise OSError(17)
        self.dirs.add(p)
    def open(self, p, mode): return RamFile(self, self._p(p), mode)
    def ilistdir(self, p):
        p = self._p(p)
        for f in self.files:
            if f.rsplit("/", 1)[0] == p: yield (f.rsplit("/", 1)[-1], 0x8000, 0)
    def chdir(self, p): pass
    def getcwd(self): return "/"

ram = RamFS()
vfs.mount(ram, "/ram")

import requests
t = time.ticks_ms()
r = requests.get("https://micropython.org/pi/v2/index.json")
print("https", r.status_code, len(r.content), "bytes", time.ticks_diff(time.ticks_ms(), t), "ms")
assert r.status_code == 200 and len(r.content) > 1000
r.close()
gc.collect(); print("mem_free", gc.mem_free())

import mip
mip.install("base64", target="/ram/lib")
mip.install("github:micropython/micropython-lib/python-stdlib/abc/abc.py", target="/ram/lib")
print(sorted(ram.files.items() and [(k, len(v)) for k, v in ram.files.items()]))
assert "lib/base64.mpy" in ram.files or "lib/base64.py" in ram.files, list(ram.files)
exec(ram.files["lib/abc.py"]); print("abc.py ok")
print("stats", lan.status("stats"))
print("TEST OK")

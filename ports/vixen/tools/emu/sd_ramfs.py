# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# A RAM filesystem mounted at /sd that behaves like vixen.Disk on the real
# card (vixen_disk.c): open() modes 'r'/'w' only (no append, no '+'), seek on
# read handles, stat() gives mode and size only, ilistdir() 4-tuples.
# Prepend to an emulator test script: the emulator has no SD card.
import vfs, io


class _RamFile(io.IOBase):
    """File contents are kept as a list of chunks (no big contiguous buffer)."""

    def __init__(self, fs, path, mode):
        self.fs, self.path, self.mode = fs, path, mode
        self.chunks = [] if "w" in mode else fs.files[path]
        self.pos = 0
        self.text = "b" not in mode

    def size(self):
        return sum(len(c) for c in self.chunks)

    def write(self, b):
        if isinstance(b, str):
            b = b.encode()
        if b:
            self.chunks.append(bytes(b))
        return len(b)

    def _read(self, n):
        out = bytearray()
        off = 0
        for c in self.chunks:
            if len(out) >= n:
                break
            if self.pos < off + len(c):
                s = max(0, self.pos - off)
                out += c[s:s + n - len(out)]
            off += len(c)
        self.pos += len(out)
        return bytes(out)

    def readinto(self, b):
        d = self._read(len(b))
        b[:len(d)] = d
        return len(d)

    def read(self, n=-1):
        if n is None or n < 0:
            n = self.size() - self.pos
        r = self._read(n)
        return r.decode() if self.text else r

    def readline(self):
        out = bytearray()
        while True:
            c = self._read(1)
            if not c:
                break
            out += c
            if c == b"\n":
                break
        return out.decode() if self.text else bytes(out)

    def __iter__(self):
        return self

    def __next__(self):
        l = self.readline()
        if not l:
            raise StopIteration
        return l

    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else (self.pos + off if whence == 1 else self.size() + off)
        return self.pos

    def close(self):
        if "w" in self.mode and self.path is not None:
            self.fs.files[self.path] = self.chunks
            self.path = None

    def ioctl(self, req, arg):
        # stream protocol for imports from this filesystem: 4 = close
        if req == 4:
            self.close()
        return 0

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


def _size(chunks):
    return sum(len(c) for c in chunks)


class SdRamFS:
    def __init__(self):
        self.files = {}
        self.dirs = {""}

    def mount(self, ro, mkfs):
        pass

    def umount(self):
        pass

    def _p(self, p):
        return p.strip("/")

    def _parent(self, p):
        return p.rsplit("/", 1)[0] if "/" in p else ""

    def stat(self, p):
        p = self._p(p)
        if p in self.dirs:
            return (0x4000, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        if p in self.files:
            return (0x8000, 0, 0, 0, 0, 0, _size(self.files[p]), 0, 0, 0)
        raise OSError(2)

    def mkdir(self, p):
        p = self._p(p)
        if p in self.dirs or p in self.files:
            raise OSError(17)
        if self._parent(p) not in self.dirs:
            raise OSError(2)
        self.dirs.add(p)

    def rmdir(self, p):
        p = self._p(p)
        if p not in self.dirs:
            raise OSError(2)
        if any(self._parent(x) == p for x in list(self.files) + list(self.dirs) if x):
            raise OSError(39)
        self.dirs.discard(p)

    def remove(self, p):
        p = self._p(p)
        if p not in self.files:
            raise OSError(2)
        del self.files[p]

    def rename(self, a, b):
        a, b = self._p(a), self._p(b)
        if b in self.files or b in self.dirs:
            raise OSError(17)
        if a in self.files:
            self.files[b] = self.files.pop(a)
        elif a in self.dirs:
            raise OSError(1)
        else:
            raise OSError(2)

    def open(self, p, mode):
        p = self._p(p)
        if "a" in mode or "+" in mode:
            raise OSError(95)
        if "w" in mode:
            if self._parent(p) not in self.dirs or p in self.dirs:
                raise OSError(2)
        elif p not in self.files:
            raise OSError(2)
        return _RamFile(self, p, mode)

    def ilistdir(self, p):
        p = self._p(p)
        if p not in self.dirs:
            raise OSError(2)
        out = []
        for d in sorted(self.dirs):
            if d and self._parent(d) == p:
                out.append((d.rsplit("/", 1)[-1], 0x4000, 0, 0))
        for f in sorted(self.files):
            if self._parent(f) == p:
                out.append((f.rsplit("/", 1)[-1], 0x8000, 0, _size(self.files[f])))
        return iter(out)

    def chdir(self, p):
        pass

    def getcwd(self):
        return "/"

    def statvfs(self, p):
        return (512, 512, 65536, 60000, 60000, 0, 0, 0, 0, 255)


sd = SdRamFS()
vfs.mount(sd, "/sd")
for _d in ("vixen-32", "vixen-32/apps", "vixen-32/apps/micropython"):
    sd.dirs.add(_d)

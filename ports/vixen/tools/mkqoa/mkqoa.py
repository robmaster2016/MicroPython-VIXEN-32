#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
"""mkqoa.py -- music for the VIXEN-32: MP3, FLAC, OGG, ... -> QOA (or IMA ADPCM WAV).

    tools/mkqoa/mkqoa.py song.mp3                    # -> song.qoa next to it
    tools/mkqoa/mkqoa.py ~/Music/album -o out/       # every audio file in a folder
    tools/mkqoa/mkqoa.py song.flac -r 22050 --mono   # smaller
    tools/mkqoa/mkqoa.py song.mp3 --adpcm            # -> song.wav, IMA ADPCM

Then copy the files to /sd/vixen-32/music/ and run "import folderplay", e.g.

    scp -O out/*.qoa vixen@<ip>:/sd/vixen-32/music/

ffmpeg decodes (anything it reads), the reference encoder qoa.h encodes
(ffmpeg reads QOA but cannot write it). qoaenc.c is compiled with $CC (default
cc) on the first run and kept in ~/.cache/vixen-mkqoa. Needs ffmpeg/ffprobe
and a C compiler (--adpcm: only ffmpeg).

Output names get '_' for spaces (the VIXEN's scp cuts names at a space).

Rate: the source's, at most 48000 -- the sound engine mixes at ~48 kHz and
streams files of up to 48 kHz. More than 2 channels are mixed down to stereo.
QOA is ~3.2 bit per sample: 1 minute of 48 kHz stereo is ~2.3 MB.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
AUDIO = (".mp3", ".flac", ".ogg", ".oga", ".opus", ".wav", ".m4a", ".aac", ".aif", ".aiff", ".wma", ".mod", ".xm")
MAX_RATE = 48000


def die(msg):
    sys.exit("mkqoa: " + msg)


def encoder():
    """Path of the compiled qoaenc (built once per source version)."""
    srcs = [os.path.join(HERE, n) for n in ("qoaenc.c", "qoa.h")]
    h = hashlib.sha256()
    for p in srcs:
        with open(p, "rb") as f:
            h.update(f.read())
    cache = os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "vixen-mkqoa")
    exe = os.path.join(cache, "qoaenc-" + h.hexdigest()[:12])
    if not os.path.exists(exe):
        os.makedirs(cache, exist_ok=True)
        cc = os.environ.get("CC", "cc")
        cmd = [cc, "-O2", "-o", exe + ".tmp", srcs[0], "-lm"]
        print("building the encoder:", " ".join(cmd))
        try:
            subprocess.run(cmd, check=True)
        except (OSError, subprocess.CalledProcessError):
            die("could not compile qoaenc.c with %r (set CC, or use --adpcm)" % cc)
        os.replace(exe + ".tmp", exe)
    return exe


def probe(path):
    """(rate, channels, seconds) of the first audio stream."""
    r = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a:0",
                        "-show_entries", "stream=sample_rate,channels:format=duration",
                        "-of", "default=noprint_wrappers=1", path],
                       capture_output=True, text=True)
    info = dict(l.split("=", 1) for l in r.stdout.splitlines() if "=" in l)
    if r.returncode or "sample_rate" not in info:
        return None
    try:
        secs = float(info.get("duration", "0"))
    except ValueError:
        secs = 0.0
    return int(info["sample_rate"]), int(info["channels"]), secs


def inputs(paths):
    for p in paths:
        if os.path.isdir(p):
            for n in sorted(os.listdir(p), key=str.lower):
                if n.lower().endswith(AUDIO) and not n.startswith("."):
                    yield os.path.join(p, n)
        elif os.path.isfile(p):
            yield p
        else:
            print("mkqoa: not found:", p, file=sys.stderr)


def convert(src, dst, rate, ch, adpcm, enc):
    pcm = ["ffmpeg", "-v", "error", "-nostdin", "-i", src, "-map", "0:a:0", "-ac", str(ch), "-ar", str(rate)]
    if adpcm:
        subprocess.run(pcm + ["-c:a", "adpcm_ima_wav", "-y", dst], check=True)
        return
    dec = subprocess.Popen(pcm + ["-f", "s16le", "-"], stdout=subprocess.PIPE)
    with open(dst + ".part", "wb") as out:
        e = subprocess.run([enc, str(ch), str(rate)], stdin=dec.stdout, stdout=out)
    dec.stdout.close()
    if dec.wait() or e.returncode:
        os.remove(dst + ".part")
        raise subprocess.CalledProcessError(dec.returncode or e.returncode, "ffmpeg | qoaenc")
    os.replace(dst + ".part", dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 epilog="Files go to /sd/vixen-32/music/; play them with 'import folderplay'.")
    ap.add_argument("input", nargs="+", help="audio files or folders")
    ap.add_argument("-o", "--out", help="output folder (default: next to each input)")
    ap.add_argument("-r", "--rate", type=int, help="sample rate (default: the source's, at most 48000)")
    ap.add_argument("--mono", action="store_true", help="mix down to one channel (half the size)")
    ap.add_argument("--adpcm", action="store_true", help="IMA ADPCM WAV instead of QOA (4 bit/sample, no compiler)")
    ap.add_argument("-f", "--force", action="store_true", help="overwrite existing outputs")
    a = ap.parse_args()

    for tool in ("ffmpeg", "ffprobe"):
        if not shutil.which(tool):
            die(tool + " not found (install ffmpeg)")
    if a.rate is not None and not 1000 <= a.rate <= MAX_RATE:
        die("--rate must be 1000..%d (the sound engine streams up to 48 kHz)" % MAX_RATE)
    enc = None if a.adpcm else encoder()
    if a.out:
        os.makedirs(a.out, exist_ok=True)
    ext = ".wav" if a.adpcm else ".qoa"

    done = failed = 0
    for src in inputs(a.input):
        info = probe(src)
        if not info:
            print("skip (no audio): " + src)
            continue
        srate, sch, secs = info
        rate = a.rate or min(srate, MAX_RATE)
        ch = 1 if a.mono else min(sch, 2)
        # no spaces: the card's scp cuts a name at the first one
        base = os.path.splitext(os.path.basename(src))[0].replace(" ", "_") + ext
        dst = os.path.join(a.out or os.path.dirname(src), base)
        if os.path.abspath(dst) == os.path.abspath(src):
            print("skip (output would overwrite the input): " + src)
            continue
        if os.path.exists(dst) and not a.force:
            print("skip (exists, -f overwrites): " + dst)
            continue
        print("%s  (%d Hz %dch, %d:%02d)  ->  %s  %d Hz %s ... " % (
            src, srate, sch, int(secs) // 60, int(secs) % 60, dst, rate, "stereo" if ch == 2 else "mono"),
            end="", flush=True)
        try:
            convert(src, dst, rate, ch, a.adpcm, enc)
        except (OSError, subprocess.CalledProcessError) as e:
            print("FAILED (%s)" % e)
            failed += 1
            continue
        mb = os.path.getsize(dst) / 1e6
        print("%.2f MB" % mb)
        done += 1
    print("%d converted%s" % (done, ", %d failed" % failed if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

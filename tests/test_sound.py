# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# sound.wav: what the loader leaves in chip RAM for PCM and IMA ADPCM files.
# The PCM engine reads its samples from WAV_BASE (left) and WAV_BASE + cap
# (right); this test reads them back and compares them with what the file says.
import machine
import sound
import struct

WAV_BASE = 0x40000000 + 0x380000 + 0x11000
WAV_MAX = 0x7F0000 - 0x391000

fails = 0


def check(name, ok):
    global fails
    print(("ok   " if ok else "FAIL ") + name)
    if not ok:
        fails += 1


def wav(fmt, data, extra=b""):
    return (b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + len(extra) + 8 + len(data)) + b"WAVE"
            + b"fmt " + struct.pack("<I", len(fmt)) + fmt + extra
            + b"data" + struct.pack("<I", len(data)) + data)


def pcm_fmt(ch, rate, bits):
    return struct.pack("<HHIIHH", 1, ch, rate, rate * ch * bits // 8, ch * bits // 8, bits)


def stored16(ch, n):
    cap = (WAV_MAX // ch) & ~3
    return [[machine.mem16[WAV_BASE + c * cap + 2 * i] for i in range(n)] for c in range(ch)]


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def wave(n, period, amp, phase=0):
    # a triangle: no float needed
    out = []
    for i in range(n):
        t = (i * 4 + phase) % (4 * period)
        v = t if t < 2 * period else 4 * period - t
        out.append((v - period) * amp // period)
    return out


# ---- IMA ADPCM: an encoder and a decoder, rounding as ffmpeg does ---------------------------

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
        253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
        3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
        11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
        32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8]


def expand(st, nib):
    diff = ((2 * (nib & 7) + 1) * STEP[st[1]]) >> 3     # as ffmpeg
    p = st[0] - diff if nib & 8 else st[0] + diff
    st[0] = max(-32768, min(32767, p))
    st[1] = max(0, min(88, st[1] + IDX[nib & 7]))
    return st[0]


def encode_nib(st, x):
    step = STEP[st[1]]
    d = x - st[0]
    nib = 8 if d < 0 else 0
    d = abs(d)
    if d >= step:
        nib |= 4
        d -= step
    if d >= step >> 1:
        nib |= 2
        d -= step >> 1
    if d >= step >> 2:
        nib |= 1
    expand(st, nib)
    return nib


def adpcm(chans, align):
    """Encode lists of samples (one per channel) as IMA ADPCM WAV data.
    Returns (data, what a decoder makes of it per channel, samples per block)."""
    ch = len(chans)
    hd = 4 * ch
    spb = (align - hd) * 2 // ch + 1
    n = len(chans[0])
    data = bytearray()
    st = [[0, 0] for _ in range(ch)]
    for b in range(0, n, spb):
        m = min(spb, n - b)
        m = 1 + (m - 1) // 8 * 8                # whole 8-sample groups only
        for c in range(ch):
            st[c][0] = chans[c][b]
            data += struct.pack("<hBB", st[c][0], st[c][1], 0)
        for g in range(1, m, 8):
            for c in range(ch):
                nibs = [encode_nib(st[c], chans[c][b + g + k]) for k in range(8)]
                data += bytes(nibs[k] | (nibs[k + 1] << 4) for k in range(0, 8, 2))
    # decode the stream again with the reference decoder (what the file says)
    ref = [[] for _ in range(ch)]
    pos = 0
    while pos + hd <= len(data):
        blk = data[pos:pos + align]
        pos += align
        st = [[struct.unpack_from("<h", blk, 4 * c)[0], blk[4 * c + 2]] for c in range(ch)]
        for c in range(ch):
            ref[c].append(st[c][0])
        for g in range((len(blk) - hd) // hd):
            for c in range(ch):
                for k in range(4):
                    v = blk[hd + g * hd + 4 * c + k]
                    ref[c].append(expand(st[c], v & 15))
                    ref[c].append(expand(st[c], v >> 4))
    return bytes(data), ref, spb


def adpcm_fmt(ch, rate, align, spb):
    return struct.pack("<HHIIHHHH", 0x11, ch, rate, rate * align // spb, align, 4, 2, spb)


# ---- PCM ------------------------------------------------------------------

n = 3000
src = wave(n, 50, 20000)
sound.wav.play(wav(pcm_fmt(1, 22050, 16), struct.pack("<%dh" % n, *src)))
check("pcm 16 bit mono", [s16(v) for v in stored16(1, n)[0]] == src)

l, r = wave(n, 40, 15000), wave(n, 70, -9000)
il = []
for i in range(n):
    il += (l[i], r[i])
sound.wav.play(wav(pcm_fmt(2, 44100, 16), struct.pack("<%dh" % (2 * n), *il)))
got = stored16(2, n)
check("pcm 16 bit stereo", [s16(v) for v in got[0]] == l and [s16(v) for v in got[1]] == r)

b8 = bytes((v >> 8) + 128 for v in src)
sound.wav.play(wav(pcm_fmt(1, 8000, 8), b8))
mem = [machine.mem8[WAV_BASE + i] for i in range(n)]
check("pcm 8 bit mono", [(m - 256 if m > 127 else m) for m in mem] == [(v >> 8) for v in src])

sound.wav.play(wav(pcm_fmt(1, 96000, 16), struct.pack("<%dh" % n, *src)))
want = [(src[2 * i] + src[2 * i + 1]) // 2 for i in range(n // 2)]
got = [s16(v) for v in stored16(1, n // 2)[0]]
check("pcm 96 kHz decimated", all(abs(a - b) <= 1 for a, b in zip(got, want)))

# ---- IMA ADPCM ------------------------------------------------------------

n = 5000
src = wave(n, 60, 24000)
data, ref, spb = adpcm([src], 256)
sound.wav.play(wav(adpcm_fmt(1, 22050, 256, spb), data))
got = [s16(v) for v in stored16(1, len(ref[0]))[0]]
check("adpcm mono == reference decoder (%d samples)" % len(ref[0]), got == ref[0])
# IMA starts with the smallest step: skip the first samples while it adapts
check("adpcm mono close to the source", max(abs(a - b) for a, b in zip(got[16:], src[16:])) < 1000)
check("adpcm mono playing", sound.wav.playing())

l, r = wave(n, 45, 20000), wave(n, 90, -12000, 7)
data, ref, spb = adpcm([l, r], 1024)
fact = b"fact" + struct.pack("<II", 4, len(ref[0]) - 3)    # the last 3 are padding
sound.wav.play(wav(adpcm_fmt(2, 44100, 1024, spb), data, fact))
got = stored16(2, len(ref[0]) - 3)
check("adpcm stereo == reference decoder",
      [s16(v) for v in got[0]] == ref[0][:-3] and [s16(v) for v in got[1]] == ref[1][:-3])

# 1000 samples in blocks of 1009: one short block, no fact chunk
data, ref, spb = adpcm([src[:1000]], 512)
sound.wav.play(wav(adpcm_fmt(1, 11025, 512, spb), data))
got = [s16(v) for v in stored16(1, len(ref[0]))[0]]
check("adpcm short last block (%d samples)" % len(ref[0]), got == ref[0] and len(ref[0]) == 993)

for name, blob in (("bad block align", wav(adpcm_fmt(1, 22050, 250, 493), data)),
                   ("bad samples per block", wav(adpcm_fmt(1, 22050, 512, 1000), data)),
                   ("adpcm 3 bit", wav(adpcm_fmt(1, 22050, 512, spb)[:14] + b"\x03\x00\x02\x00" + struct.pack("<H", spb), data))):
    try:
        sound.wav.play(blob)
        check("rejects " + name, False)
    except ValueError:
        check("rejects " + name, True)

# ---- streaming through the ring ---------------------------------------------
import time

# stereo ADPCM, forced through the ring: the file fits, so the ring holds it all
data, ref, spb = adpcm([l, r], 1024)
sound.wav.play(wav(adpcm_fmt(2, 44100, 1024, spb), data, fact), stream=True)
check("stream playing", sound.wav.playing())
got = stored16(2, len(ref[0]) - 3)
check("stream adpcm stereo == reference decoder",
      [s16(v) for v in got[0]] == ref[0][:-3] and [s16(v) for v in got[1]] == ref[1][:-3])
time.sleep(0.3)                                 # 4994 frames at 44.1 kHz: 0.11 s
check("stream ends by itself", not sound.wav.playing())

# a ramp (sample = frame number) through a ring of 4096 frames: 4 s at 8 kHz
import array
RING = 4096
ramp = bytes(array.array("h", range(32000)))
t0 = time.ticks_ms()
sound.wav.play(wav(pcm_fmt(1, 8000, 16), ramp), stream=True, _ring=RING)
time.sleep(1.5)
slots = stored16(1, RING)[0]
check("ring: every slot holds a frame of its own slot", all(v % RING == i for i, v in enumerate(slots)))
check("ring: wrapped and refilled (oldest frame %d)" % min(slots), min(slots) >= RING)
check("ring: fresh (newest frame %d, played ~12000)" % max(slots), 12000 <= max(slots) < 12000 + RING)
while sound.wav.playing() and time.ticks_diff(time.ticks_ms(), t0) < 9000:
    time.sleep(0.05)
dt = time.ticks_diff(time.ticks_ms(), t0)
check("ring: ends after the file (%d ms for 4000 ms)" % dt, 4000 <= dt < 4600)

# loop=True keeps streaming past the end
sound.wav.play(wav(pcm_fmt(1, 8000, 16), ramp[:2 * 2 * RING]), stream=True, loop=True, _ring=RING)
time.sleep(1.5)                                 # 2 rings = 8192 frames, ~1 s
check("ring: loop keeps playing", sound.wav.playing())
slots = stored16(1, RING)[0]
check("ring: loop restarts the file", all(v % RING == i % RING for i, v in enumerate(slots)) and max(slots) < 2 * RING)
sound.wav.stop()
check("ring: stop", not sound.wav.playing())

# ---- QOA (sound.qoa) ----------------------------------------------------------
# Made with the reference encoder (qoa.h, phoboslab/qoa); the *_SHA values are
# SHA-256 of what qoa.h's decoder makes of them, one 16-bit LE array per channel.
# Mono: 6000 samples at 22.05 kHz. Stereo: 12345 samples at 44.1 kHz -- two full
# frames and a short third one whose last slice holds 5 samples. 3ch: 100.
QOA_MONO = 'cW9hZgAAF3ABAFYiFAAIGAAAAAAAAAAAAAAAAOAAQABBow2SywSQSB3BBSg0kALZGuqoCBImn80WotptiC3XSxUWyRd//9ZSJBIBh1tgpRQlAiDRSTFJEiTbdrrDSCRNK9/sBKZaRAEmWQtptEGbfS/bZSSJ7/9ZIqabaRF2uo4xIgiC3W6SjDShBbbbIKSiMAM81toyJQIyG2ySoMokCzdprBAQTVb5PmhxLJYhMlgwhMMlCW7bTTDQBBp7tpZSOaLBA0VNLDI8kA7b3wSoEDZtfvFai0kRN3usISaaRBtG2SlNFimWbUbbRSSBbtfDOGTTaAG+2tI9tlmG7++ypjW0J7/9JSUmQAs11toqRQBASy2AhFIIQz/95ZCQRLvvR1tBLJQBMElEBFFEEW7a2UZABJZrrpZSSSRIIhgoqJZDii3z3SyimkZdddbIimUhQMUECCRYREtO7aClEiTT3U5pAkmCZ77ZQwLDRBU28spNJomm2+aClEkkBb/LqKEEREmu3tgLSSNJVS2DBJJEhUttrBBSaLddTm1BaTBIgkFAhIsMELX+a0DIIYptd4pgTTLCFEohJSJMkjXf3WySCULp57rcU00UQgMACJZYYE1LfzWAgFQ6+U9bSWWyhDFRRBRZSAN9/tlEtAlQ3a6AFkWyhft7LQEEREl9+2lLTRJA1S2nBMssBVbrbBIABJLdR3kpaaKAAkBClBloka/fWUoJDBprvdLUTLaJBEJwwYZIom3/3W2C00Zdd55KWm0UQolNBRTKQE9L++amkyKbeU9ZCk0mCRBCQKKZSAN3+2lCEgCy7a6QlEmyjP9rZCSURAs9/uuLaZxAWW2ClJMsBUvfdTKCILbtTvkjLaJAAkhAIJMpAXfea0JJBLLrvboUSbKJIFOOCKRMFKb++aWGCULddrrMWmkUQoghGDDKIC1L7zWQQGHW7UtYCy2USIJAQKJZSEN322tCiimxXa6SIk0ywBpbBEMiRAF1/npLTKBCGW2QVItjGU7/FDRQBBpvR2sqLZKNAklEVFFlCK/a20aZAZZrtdaESZSIJNFFJKRIIS7eeaKqi0JptdLCi0igOIOODLbTQn9LbXSWmmWzx0LYSmWEQIYKQJKRJAuv10M5IkTb7/+GpkyiAr5pICgkQIE2u0CLYRBCW22wEpFAm047ZKIgCJZ5S1gpTHEIlhhCgljggza6wzaIBJ59/5akPbbFIEFBSTY1iq//3aWSSTdd77ZE020iNIgIxSZSAns//baGSGzzezdCU02yCLJBOJTKIA9+/tkwkgw7XeKQljyxVt7rCUimQAEttgCLHEI+HzzSBFIA3zvdZCSiUIddOtoSTCIhttswUlhiBW+2yTCMKYbtrYaiORQIhqoo4SI0iG7TWWCQGDpdbZaCiiUMJoAJTSbRMX0v/3yw1Vx7bSJMk22ySBJCJKSRImv331gggkw6bSyEpCyiJttLKCiQJAt10NKSZQghRSySgkkS3SdLYASMKabZIkBCJJEEksESlNIE3bbWwBKCIbdrZCEkFJQoBFCCJIoWiw0w0w266xZKCSRjRBKCFKKAMO1skpIYEAa7yzFpthxAdNrYUmWIIttsMgBBIhgq2yAFIkkQWSbBSikCBptpJsFBFNnu2hgolsIgmGQYNCkwjD77tbAIIit/u2oqbTIsEw2UJNpIgyd/97ZSSZdvL3lmTbbRjv0mgtttIC//7TAiwSLLrtLBNJJFn+9toKQ4okGXS0MtljILdd5bEEUIN290sCDKKMU3zyTJlIgTTTrYCkygVb9rNgBKFMd319AxlsIgWKGkpjWkRNt9pbRAMF13v1kRabQxCQQUotMpGDt/vdJBRFPPSlsgQSQJEhk0BhtMoDb/+zpECTLvf/paRKRICl9MpJRFjExTawlNlDCp9/9bi22RQvu0kBJRJBtG7WQllIUwzUbJCikSIr5rOgJKIiv//9pEpIoQGGGkFEWERFdtbIaIQhs12tkqLRI8ECChJpttA0dttNDVQbY5S1toLLRJJphAjIJIkm7e7ULBCYZrfb4KQZbCEGllBKRFkgDb322ggkIbptrYQmWESn+vomBSBA1D7aWstkgCy0bISUyyAtttQMhBEEuvu8FEtJjCmCSEkkmUhLt9dDDRQhtu21sKTSJIAmgRApJFA0d9tbYZBFFdR20FLaSKEJhEUoMpAjX/bULaaJLp57rCRbLYwhkgJKRJhEzbe+awGEJdPPrZSmkyON98ESTbbU1Le+zJpoiGK1ZJASiSTNdbRUAJAA03vuhFJsoACSGEhkUygL7/NDIBQhmt92sLLTBIY0QAhFhkk0d/b4bJDDNbS20FZaZKAFFAjBhlAje/bUbDTBNNdtbCSTaLNBglBRZBkka77a2mAEZZ57tJSk0mQwkEpGRSJA1Hfb0VFnCWz0LoRymiTr/6RgoJJFi291hFNhgiGGwUcEmWMp95rJKIQhl13npySZRAimgUYopjA0d75pZYLJdNSutpLYZJIgBChBIokTX7azrbAYr7//6CSZaKAAEgJKJIomK/baGUU0TotddBEkkiRAkghJLHIJtH3eSYlAVQ60pbhkWgDd7pRoiBJEtvu0BBosEmC2SCkkyWcrfdLIIRQps111uCLKIzAQQEpNtlC0drrZYJaobdRtsoSRRJABFEBBEkkxa3yz5bVVFtvdqORLKCIhMJwQw8oBb//3ySE0IoppoJgkiSMs1koSTRYqs/7+4Mtgw360pZAUSCbvNoMOKSRAO9+sJFjgIyGy2ChAEAViIDcAF4DQgMmAwiDPICT/833WdAwTGyDP9vVCCSMJ0+3thSaYI0SWGUpFscwzvfNZBKLFPdO1kxTSKIAAowJFIoFR33XTZJJJbptZJiNaYRIJgIqJQ0gGbX3XUwEzBdLdDQUkiiIM1hqTTTAG83zWCokgmxWToYCSSDNfrLIxZZZAu3mpQ5FEEyKKS0UikUpv9tDCiUJJm22tCSbJIiGXQQllMUKyr7baICadbvIVpCSLBIAgogooogCXW66CbDJZdLZYKhKRKCAIBFCFAkQw26K2yw0yIYLTQASQgCHVNMIoohh1sfbeW00UgEIBiSiigSQQQAFg011us0ssUWWWkIttJkoRRQkWwwTN9/KkskAkMsttEjAolAFQy0QiggQIptbZCiJQgUuuqx4RQgXX30xBFkkDIbDKMSiABJLvmshKBC338uCBJlokzXaTCESgAJbNLJLSTQi3/ukCI0kmS66SQolDSaLLZASiiQMUc1tgoopis='
QOA_MONO_SHA = ['80d3680bed7aa4e43bfb1655b9fc1e7d8930571985bde20758127ea43f606ce8']
QOA_STEREO = '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'
QOA_STEREO_SHA = ['aa023ecf9abfe81f2004133c1fb129d6305c4cb0b304fca9419890ae9173a2b6', 'dc70a82014633e337d30828468c5709b9ce5dc622979fe64f35c9c93a270eb70']
QOA_3CH = 'cW9hZgAAAGQDAFYiAGQAsAAAAAAAAAAAAAAAAOAAQAAAAAAAAAAAAAAAAADgAEAAAAAAAAAAAAAAAAAA4ABAAEGaYLDBYJBIQZpgsMFgkEhBmmCwwWCQSB6hRQASCSCQHqFFABIJIJAeoUUAEgkgkBCBJJbdNrbBEIEklt02tsEQgSSW3Ta2wRKKRABJIIKREopEAEkggpESikQASSCCkRiSCIZptbpoGJIIhmm1umgYkgiGabW6aA=='

import binascii, hashlib, uctypes

def ring_sha(ch, n):
    cap = (WAV_MAX // ch) & ~3
    return [binascii.hexlify(hashlib.sha256(bytes(uctypes.bytearray_at(WAV_BASE + c * cap, 2 * n)))
                             .digest()).decode() for c in range(ch)]

QM = binascii.a2b_base64(QOA_MONO)
QS = binascii.a2b_base64(QOA_STEREO)
sound.qoa.play(QM)
check("qoa mono == qoa.h", ring_sha(1, 6000) == QOA_MONO_SHA)
check("qoa playing", sound.qoa.playing() and sound.wav.playing())
sound.qoa.play(QS)
check("qoa stereo, short last frame and slice == qoa.h", ring_sha(2, 12345) == QOA_STEREO_SHA)
sound.qoa.play(QS, stream=True)
check("qoa stereo streamed == qoa.h", ring_sha(2, 12345) == QOA_STEREO_SHA)
sound.qoa.stop()
check("qoa stop", not sound.qoa.playing())

# a decoder in Python (qoa.h's algorithm), checked against qoa.h, for the ring test
DQ = [[1, -1, 3, -3, 5, -5, 7, -7], [5, -5, 18, -18, 32, -32, 49, -49],
      [16, -16, 53, -53, 95, -95, 147, -147], [34, -34, 113, -113, 203, -203, 315, -315],
      [63, -63, 210, -210, 378, -378, 588, -588], [104, -104, 345, -345, 621, -621, 966, -966],
      [158, -158, 528, -528, 950, -950, 1477, -1477], [228, -228, 760, -760, 1368, -1368, 2128, -2128],
      [316, -316, 1053, -1053, 1895, -1895, 2947, -2947], [422, -422, 1405, -1405, 2529, -2529, 3934, -3934],
      [548, -548, 1828, -1828, 3290, -3290, 5117, -5117], [696, -696, 2320, -2320, 4176, -4176, 6496, -6496],
      [868, -868, 2893, -2893, 5207, -5207, 8099, -8099], [1064, -1064, 3548, -3548, 6386, -6386, 9933, -9933],
      [1286, -1286, 4288, -4288, 7718, -7718, 12005, -12005], [1536, -1536, 5120, -5120, 9216, -9216, 14336, -14336]]


def qoa_decode(b):
    n, ch = int.from_bytes(b[4:8], "big"), b[8]
    out = [[] for _ in range(ch)]
    p = 8
    while p < len(b) and len(out[0]) < n:
        fs = int.from_bytes(b[p + 4:p + 6], "big")
        p += 8
        lms = []
        for c in range(ch):
            h = list(struct.unpack(">4h", b[p:p + 8]))
            w = list(struct.unpack(">4h", b[p + 8:p + 16]))
            lms.append((h, w))
            p += 16
        for si in range(0, fs, 20):
            for c in range(ch):
                hi, lo = struct.unpack(">II", b[p:p + 8])
                p += 8
                dq, (h, w) = DQ[hi >> 28], lms[c]
                bits = ((hi & 0xFFFFFFF) << 32) | lo     # 60 bits of residuals
                for i in range(min(20, fs - si)):
                    r = dq[(bits >> (57 - 3 * i)) & 7]
                    v = (w[0] * h[0] + w[1] * h[1] + w[2] * h[2] + w[3] * h[3]) >> 13
                    v = -32768 if v + r < -32768 else 32767 if v + r > 32767 else v + r
                    d = r >> 4
                    for k in range(4):
                        w[k] += -d if h[k] < 0 else d
                    h.pop(0)
                    h.append(v)
                    out[c].append(v)
    return out


ref = qoa_decode(QM)[0]
check("python qoa decoder == qoa.h",
      binascii.hexlify(hashlib.sha256(bytes(array.array("h", ref))).digest()).decode() == QOA_MONO_SHA[0])

# through a ring of 2048 frames, looping: every slot holds a sample of its slot
QRING = 2048
sound.qoa.play(QM, stream=True, loop=True, _ring=QRING)
time.sleep(0.8)                                 # ~3 passes of 0.27 s
check("qoa ring: loop keeps playing", sound.qoa.playing())
slots = [s16(v) for v in stored16(1, QRING)[0]]
cand = [set(ref[(i + QRING * m) % 6000] for m in range(60)) for i in range(QRING)]
check("qoa ring: every slot from the file", all(v in cand[i] for i, v in enumerate(slots)))
sound.qoa.stop()

for name, blob, mod in (("qoa: a WAV", wav(pcm_fmt(1, 8000, 16), b"\0\0" * 100), sound.qoa),
                        ("wav: a QOA file", QM, sound.wav),
                        ("qoa: 3 channels", binascii.a2b_base64(QOA_3CH), sound.qoa),
                        ("qoa: 0 samples (streaming QOA)", QM[:4] + bytes(4) + QM[8:], sound.qoa),
                        ("qoa: bad frame size", QM[:14] + b"\xff\xff" + QM[16:], sound.qoa)):
    try:
        mod.play(blob)
        check("rejects " + name, False)
    except ValueError:
        check("rejects " + name, True)
sound.qoa.play(QS[:5000])
check("qoa: a cut file plays what is there", sound.qoa.playing())

# ---- pause / resume -----------------------------------------------------------
def until_quiet(t0, limit=9000):
    while sound.wav.playing() and time.ticks_diff(time.ticks_ms(), t0) < limit:
        time.sleep(0.01)
    return time.ticks_diff(time.ticks_ms(), t0)

t0 = time.ticks_ms()
sound.qoa.play(QM)                              # loaded: 272 ms
sound.qoa.pause()
check("pause: paused()", sound.qoa.paused() and sound.wav.paused())
time.sleep(0.5)
check("pause: still playing after its length", sound.qoa.playing())
sound.qoa.resume()
check("resume: not paused", not sound.qoa.paused())
dt = until_quiet(t0)
check("pause: the end moves by the pause (%d ms for ~780)" % dt, 760 <= dt < 900)

t0 = time.ticks_ms()
sound.wav.play(wav(pcm_fmt(1, 8000, 16), ramp), stream=True, _ring=RING)    # 4 s
time.sleep(1)
sound.wav.pause()
time.sleep(1)
slots = stored16(1, RING)[0]
check("pause: the ring stays in order", all(v % RING == i for i, v in enumerate(slots)))
check("pause: no refill past the pause (newest %d)" % max(slots), max(slots) < 8000 + RING)
sound.wav.resume()
time.sleep(1)
slots = stored16(1, RING)[0]
check("resume: streaming goes on in order", all(v % RING == i for i, v in enumerate(slots)) and max(slots) >= 12000)
dt = until_quiet(t0)
check("pause: streamed end moves by the pause (%d ms for 5000)" % dt, 5000 <= dt < 5600)
sound.wav.pause()
check("pause when nothing plays: nothing", not sound.wav.paused())
sound.qoa.play(QM, loop=True)
sound.qoa.pause()
sound.wav.stop()
check("stop ends a pause", not sound.wav.paused() and not sound.wav.playing())

sound.wav.stop()
check("stopped", not sound.wav.playing())
print("TEST OK" if not fails else "TEST FAILED (%d)" % fails)

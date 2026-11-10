#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
"""Build "Neon Grid", the example SID tune, from the score below.

    python3 compose.py ../../examples/neon_grid.sid

An original composition for this project (MIT, like the rest of it), in the
style of an 80s C64 crack intro: 125 BPM in A minor, Am-F-C-G.

  voice 1  octave-jumping 16th bass, pulse-width modulation, resonant
           low-pass filter sweeping open and shut
  voice 2  lead: pulse with PWM, a little bend into every note, delayed
           vibrato on the long ones; silent for the first four bars
  voice 3  50 Hz chord arpeggios on the offbeats, kick and snare in between
           (pitch-drop kick, noise snare), drums from bar 3

Python computes every frame -- arpeggios, vibrato, PWM, drums, the filter --
and the 6502 player only streams the register writes: per frame a list of
(register, value) pairs, $FF ends a frame, $FE loops the song. So any effect
is a few lines of Python here, and the player stays under 60 bytes.
"""
import math
import struct
import sys

LOAD = 0x1000
PAL_CLOCK = 985248
STEP = 6                     # frames per 16th: 50 Hz / 6 / 4 = 125 BPM
BAR = 16 * STEP

# ---- score -------------------------------------------------------------------
CHORDS = ["Am", "F", "C", "G"] * 3 + ["Dm", "F", "G", "E"]
CHORD_NOTES = {   # root (bass, octave 1-2), arpeggio offsets
    "Am": ("A1", (0, 3, 7)), "F": ("F1", (0, 4, 7)), "C": ("C2", (0, 4, 7)),
    "G": ("G1", (0, 4, 7)), "Dm": ("D2", (0, 3, 7)), "E": ("E2", (0, 4, 7)),
}
LEAD_FROM_BAR = 4
LEAD = [   # (note, 16ths); "r" = rest. One line per bar, from bar 5.
    ("A4", 4), ("C5", 4), ("E5", 6), ("D5", 2),
    ("C5", 4), ("A4", 4), ("F4", 4), ("A4", 2), ("C5", 2),
    ("G4", 6), ("E4", 2), ("G4", 2), ("C5", 2), ("E5", 4),
    ("D5", 8), ("B4", 4), ("G4", 2), ("B4", 2),
    ("A5", 6), ("G5", 2), ("E5", 4), ("C5", 4),
    ("F5", 6), ("E5", 2), ("C5", 4), ("A4", 4),
    ("G5", 4), ("E5", 2), ("G5", 2), ("C6", 6), ("B5", 2),
    ("B5", 4), ("A5", 2), ("G5", 2), ("D5", 8),
    ("F5", 4), ("E5", 2), ("D5", 2), ("A4", 4), ("D5", 4),
    ("C5", 4), ("A4", 2), ("C5", 2), ("F5", 6), ("E5", 2),
    ("D5", 2), ("E5", 2), ("F5", 2), ("G5", 2), ("B5", 4), ("D6", 4),
    ("E5", 12), ("G#5", 2), ("B5", 2),
]

NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_no(name):
    """"A4" -> semitones above C0."""
    return NAMES.index(name[:-1]) + 12 * int(name[-1])


def sid_freq(semis):
    hz = 16.3516 * 2 ** (semis / 12)
    return min(0xFFFF, round(hz * 16777216 / PAL_CLOCK))


# ---- frame-by-frame synthesis --------------------------------------------------
class Voice:
    def __init__(self):
        self.r = {0: 0, 1: 0, 2: 0, 3: 8, 4: 0, 5: 0, 6: 0}

    def set(self, freq=None, pw=None, ctrl=None, ad=None, sr=None):
        if freq is not None:
            self.r[0], self.r[1] = freq & 0xFF, freq >> 8
        if pw is not None:
            self.r[2], self.r[3] = pw & 0xFF, (pw >> 8) & 0x0F
        if ctrl is not None:
            self.r[4] = ctrl
        if ad is not None:
            self.r[5] = ad
        if sr is not None:
            self.r[6] = sr


def bass(v, f):
    bar, pos = divmod(f, BAR)
    step, sub = divmod(pos, STEP)
    root = note_no(CHORD_NOTES[CHORDS[bar % len(CHORDS)]][0])
    note = root + (12 if step % 2 else 0)
    pw = 0x400 + int(0x300 * math.sin(f / 37))            # slow PWM
    gate = 0 if sub == STEP - 1 else 1                     # short gap before each 16th
    v.set(freq=sid_freq(note), pw=pw, ctrl=0x40 | gate, ad=0x08, sr=0x9A)


def drum(kind, k):
    """Register settings for frame k of a drum hit, or None when it is over."""
    if kind == "kick":
        seq = [(0x81, 0x30), (0x41, 0x0E), (0x41, 0x0A), (0x41, 0x07), (0x41, 0x05), (0x40, 0x04)]
    else:   # snare
        seq = [(0x81, 0x38), (0x41, 0x10), (0x81, 0x24), (0x81, 0x20), (0x81, 0x1C), (0x80, 0x1A)]
    if k >= len(seq):
        return None
    ctrl, hi = seq[k]
    return ctrl, hi << 8


def chords_and_drums(v, f):
    bar, pos = divmod(f, BAR)
    step, sub = divmod(pos, STEP)
    beat_step = step % 4
    if bar >= 2 and beat_step in (0, 1):                   # drum on every beat
        kind = "kick" if step in (0, 8) else "snare"
        hit = drum(kind, (beat_step * STEP) + sub)
        if hit:
            ctrl, freq = hit
            v.set(freq=freq, pw=0x800, ctrl=ctrl, ad=0x08 if kind == "kick" else 0x0A, sr=0x00)
            return
        v.set(ctrl=v.r[4] & 0xFE)
        return
    if beat_step in (0, 1):
        v.set(ctrl=v.r[4] & 0xFE)                          # intro: room for the drums
        return
    # offbeat chord: arpeggio, one chord tone per frame
    root, offs = CHORD_NOTES[CHORDS[bar % len(CHORDS)]]
    base = note_no(root) + 36                              # three octaves above the bass
    k = f % len(offs)
    gate = 0 if (beat_step == 3 and sub == STEP - 1) else 1
    pw = 0x600 + int(0x500 * math.sin(f / 23))
    v.set(freq=sid_freq(base + offs[k]), pw=pw, ctrl=0x40 | gate, ad=0x02, sr=0xA6)


def lead_timeline():
    """[(start frame, frames, note)] for the lead, from bar LEAD_FROM_BAR."""
    out, f = [], LEAD_FROM_BAR * BAR
    for name, length in LEAD:
        out.append((f, length * STEP, None if name == "r" else note_no(name)))
        f += length * STEP
    return out


LEAD_TL = lead_timeline()


def lead(v, f):
    for start, length, note in LEAD_TL:
        if start <= f < start + length:
            break
    else:
        v.set(ctrl=v.r[4] & 0xFE, ad=0x09, sr=0xA9)
        return
    k = f - start
    if note is None or k == length - 1:
        v.set(ctrl=v.r[4] & 0xFE)                          # release before the next note
        return
    bend = -0.6 * max(0, 3 - k) / 3                        # slide up into the note
    vib = 0.25 * math.sin((k - 12) * 0.75) if k > 12 else 0
    pw = 0x300 + int(0x200 * math.sin(f / 17))
    v.set(freq=sid_freq(note + bend + vib), pw=pw, ctrl=0x41, ad=0x09, sr=0xA9)


def song():
    """Register writes per frame: [[(reg, value), ...], ...]"""
    voices = [Voice(), Voice(), Voice()]
    glob = {0x15: 0, 0x16: 0, 0x17: 0xA1, 0x18: 0x1F}       # res 10, filter voice 1; low-pass, vol 15
    frames = []
    prev = {}
    total = BAR * len(CHORDS)
    for f in range(total):
        bass(voices[0], f)
        lead(voices[1], f)
        chords_and_drums(voices[2], f)
        cutoff = 0x180 + int(0x280 * (0.5 - 0.5 * math.cos(2 * math.pi * f / (4 * BAR))))
        glob[0x15], glob[0x16] = cutoff & 7, cutoff >> 3
        state = {}
        for vi, v in enumerate(voices):
            for r in (5, 6, 0, 1, 2, 3, 4):                # ADSR and pitch before the gate
                state[vi * 7 + r] = v.r[r]
        state.update(glob)
        writes = [(r, val) for r, val in state.items() if f == 0 or prev.get(r) != val]
        frames.append(writes)
        prev = state
    return frames


# ---- player and PSID -----------------------------------------------------------
def player(data_addr):
    """init: ptr = data. play: write (reg, value) pairs to $D400+reg until
    $FF (end of frame, ptr += bytes read) or $FE (end of song: ptr = data)."""
    lo, hi = data_addr & 0xFF, data_addr >> 8
    PTR = 0xFB
    init, play = LOAD + 6, LOAD + 15
    c = bytearray([0x4C, init & 0xFF, init >> 8, 0x4C, play & 0xFF, play >> 8])
    c += bytes([0xA9, lo, 0x85, PTR, 0xA9, hi, 0x85, PTR + 1, 0x60])      # init
    assert LOAD + len(c) == play
    c += bytes([0xA0, 0x00])                                              # ldy #0
    loop = LOAD + len(c)
    c += bytes([0xB1, PTR, 0xC8])                                         # lda (ptr),y; iny
    c += bytes([0xC9, 0xFF, 0xF0, 0])                                     # cmp #$FF; beq endframe
    beq_end = len(c) - 1
    c += bytes([0xC9, 0xFE, 0xF0, 0])                                     # cmp #$FE; beq restart
    beq_restart = len(c) - 1
    c += bytes([0xAA, 0xB1, PTR, 0xC8, 0x9D, 0x00, 0xD4])                 # tax; lda (ptr),y; iny; sta $D400,x
    c += bytes([0x4C]) + struct.pack("<H", loop)                          # jmp loop
    c[beq_end] = len(c) - (beq_end + 1)
    c += bytes([0x98, 0x18, 0x65, PTR, 0x85, PTR, 0x90, 0x02, 0xE6, PTR + 1, 0x60])   # ptr += y; rts
    c[beq_restart] = len(c) - (beq_restart + 1)
    c += bytes([0xA9, lo, 0x85, PTR, 0xA9, hi, 0x85, PTR + 1, 0x60])      # ptr = data; rts
    return bytes(c)


def build():
    frames = song()
    data = bytearray()
    for writes in frames:
        for r, v in writes:
            data += bytes([r, v])
        data.append(0xFF)
    data[-1] = 0xFE                       # the last frame loops instead of ending
    data_addr = LOAD + len(player(0x2000))
    code = player(data_addr) + bytes(data)
    assert LOAD + len(code) <= 0xD000, "song too long for the RAM below $D000"
    return code, len(frames)


def psid(code, title, author, released):
    # init = LOAD (jmp init), play = LOAD + 3 (jmp play); flags 0x0014: PAL, 6581
    h = struct.pack(">4sHHHHHHHI", b"PSID", 2, 0x7C, 0, LOAD, LOAD + 3, 1, 1, 0) \
        + title.encode().ljust(32, b"\0") + author.encode().ljust(32, b"\0") \
        + released.encode().ljust(32, b"\0") + struct.pack(">HBBH", 0x0014, 0, 0, 0)
    assert len(h) == 0x7C
    return h + struct.pack("<H", LOAD) + code


if __name__ == "__main__":
    total = sum(l for _, l in LEAD)
    assert total == 16 * (len(CHORDS) - LEAD_FROM_BAR), total
    code, n = build()
    out = psid(code, "Neon Grid", "Robmaster", "2026 MicroPython-VIXEN-32")
    open(sys.argv[1] if len(sys.argv) > 1 else "neon_grid.sid", "wb").write(out)
    print("%d bytes, loop %.1f s" % (len(out), n / 50))

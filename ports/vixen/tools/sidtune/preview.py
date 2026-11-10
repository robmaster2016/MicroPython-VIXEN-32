#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
"""Rough PC preview of a SID tune: render the register writes that
tools/sidtest/sidplay logs into a WAV file.

    SIDLOG=writes.txt ../sidtest/sidplay tune.sid 0 40
    python3 preview.py writes.txt preview.wav

A simple model -- pulse/saw/triangle/noise oscillators, the SID's ADSR
rates, a state-variable filter with a rough 6581 cutoff curve, master
volume; no ring/sync -- to judge notes and groove on the PC. It does not
sound like the real chip.
"""
import array
import math
import random
import sys
import wave

RATE = 22050
CLOCK = 985248
ATTACK_MS = (2, 8, 16, 24, 38, 56, 68, 80, 100, 250, 500, 800, 1000, 3000, 5000, 8000)


def render(log, out):
    writes = []
    for line in open(log):
        t, r, v = line.split()
        writes.append((float(t) / 1000, int(r), int(v)))
    end = writes[-1][0] + 1.0
    regs = [0] * 32
    phase = [0.0] * 3
    env = [0.0] * 3
    stage = ["rel"] * 3          # att, dec (then sustain), rel
    gate = [0] * 3
    noise = [0.0] * 3
    low = band = 0.0
    samples = array.array("h")
    wi = 0
    for n in range(int(end * RATE)):
        t = n / RATE
        while wi < len(writes) and writes[wi][0] <= t:
            _, r, v = writes[wi]
            regs[r] = v
            if r in (4, 11, 18):
                vc = r // 7
                if v & 1 and not gate[vc]:
                    stage[vc] = "att"
                elif not v & 1:
                    stage[vc] = "rel"
                gate[vc] = v & 1
            wi += 1
        mix = 0.0
        filt_in = 0.0
        route = regs[0x17] & 7
        for vc in range(3):
            o = vc * 7
            freq = (regs[o] | regs[o + 1] << 8) * CLOCK / 16777216
            ctrl, ad, sr = regs[o + 4], regs[o + 5], regs[o + 6]
            sustain = (sr >> 4) / 15
            if stage[vc] == "att":
                env[vc] += 1 / (ATTACK_MS[ad >> 4] / 1000 * RATE)
                if env[vc] >= 1:
                    env[vc], stage[vc] = 1.0, "dec"
            elif stage[vc] == "dec":
                if env[vc] > sustain:
                    env[vc] -= 1 / (3 * ATTACK_MS[ad & 15] / 1000 * RATE)
            else:
                env[vc] = max(0.0, env[vc] - 1 / (3 * ATTACK_MS[sr & 15] / 1000 * RATE))
            if env[vc] <= 0:
                continue
            p0 = phase[vc]
            phase[vc] = (p0 + freq / RATE) % 1.0
            p = phase[vc]
            if ctrl & 0x80:
                if p < p0:
                    noise[vc] = random.uniform(-1, 1)
                s = noise[vc]
            elif ctrl & 0x40:
                pw = ((regs[o + 3] & 15) << 8 | regs[o + 2]) / 4096
                s = 1.0 if p < pw else -1.0
            elif ctrl & 0x20:
                s = 2 * p - 1
            elif ctrl & 0x10:
                s = 4 * p - 1 if p < 0.5 else 3 - 4 * p
            else:
                s = 0.0
            if route >> vc & 1:
                filt_in += s * env[vc]
            else:
                mix += s * env[vc]
        mode = regs[0x18] & 0x70
        if route and mode:
            cutoff = (regs[0x16] << 3) | (regs[0x15] & 7)
            fc = 200 * 60 ** (cutoff / 2047)                 # ~200 Hz .. 12 kHz
            f = 2 * math.sin(math.pi * min(fc, RATE / 6) / RATE)
            q = 1.4 - 1.2 * (regs[0x17] >> 4) / 15
            low += f * band
            high = filt_in - low - q * band
            band += f * high
            if mode & 0x10:
                mix += low
            if mode & 0x20:
                mix += band
            if mode & 0x40:
                mix += high
        else:
            mix += filt_in
        mix *= (regs[0x18] & 15) / 15 / 3.6      # headroom for the resonance
        samples.append(int(max(-1.0, min(1.0, mix)) * 26000))
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(samples.tobytes())


if __name__ == "__main__":
    render(sys.argv[1], sys.argv[2])

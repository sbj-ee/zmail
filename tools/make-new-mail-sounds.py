#!/usr/bin/env python3
"""Generate the extra new-mail sounds in assets/sounds/: original sounds made
for zmail and dedicated to the public domain (CC0-1.0). Deterministic output.
Usage: tools/make-new-mail-sounds.py [outdir]

Each is short (under 1.5 s), 44.1 kHz, 16-bit mono WAV:
  bell.wav     one soft bell stroke
  marimba.wav  three rising wooden notes
  glass.wav    a high, clear ding
  knock.wav    two soft knocks on wood
  drop.wav     a water drop
  harp.wav     four plucked strings, upward
"""
import math
import os
import random
import struct
import sys
import wave

SR = 44100
TAU = 2 * math.pi
outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(__file__), "..", "assets", "sounds")


def attack(t, a=0.004):
    return min(1.0, t / a)


def partials(buf, start, parts, gain=1.0):
    """Add decaying sine partials (freq, amp, decay/s) from `start` seconds."""
    s0 = int(start * SR)
    for i in range(s0, len(buf)):
        t = (i - s0) / SR
        buf[i] += gain * attack(t) * sum(
            a * math.exp(-t * d) * math.sin(TAU * f * t) for f, a, d in parts)


def bell():
    buf = [0.0] * int(SR * 1.4)
    f = 659.25  # E5; the partials of a struck bell are not harmonic
    partials(buf, 0.0, [(f * 0.5, 0.25, 2.0), (f, 1.0, 3.0), (f * 2.0, 0.45, 4.0),
                        (f * 2.76, 0.30, 5.5), (f * 4.07, 0.16, 8.0), (f * 5.4, 0.08, 11.0)])
    return buf


def marimba():
    buf = [0.0] * int(SR * 0.9)
    for start, f in [(0.0, 523.25), (0.11, 659.25), (0.22, 783.99)]:  # C5 E5 G5
        partials(buf, start, [(f, 1.0, 9.0), (f * 4.0, 0.28, 30.0), (f * 10.0, 0.06, 60.0)])
    return buf


def glass():
    buf = [0.0] * int(SR * 1.0)
    f = 2093.0  # C7
    partials(buf, 0.0, [(f, 1.0, 5.0), (f * 2.32, 0.22, 9.0), (f * 4.25, 0.07, 16.0)])
    return buf


def knock():
    buf = [0.0] * int(SR * 0.5)
    rng = random.Random(7)
    for start, gain in [(0.0, 1.0), (0.16, 0.8)]:
        partials(buf, start, [(190.0, 1.0, 38.0), (410.0, 0.5, 55.0), (980.0, 0.18, 90.0)], gain)
        s0, lp = int(start * SR), 0.0
        for i in range(s0, min(len(buf), s0 + int(0.03 * SR))):  # the tap itself
            t = (i - s0) / SR
            lp += 0.25 * (rng.uniform(-1, 1) - lp)
            buf[i] += gain * 0.5 * lp * math.exp(-t * 180.0)
    return buf


def drop():
    buf = [0.0] * int(SR * 0.45)
    for start, f0, f1, gain in [(0.0, 620.0, 1500.0, 1.0), (0.13, 900.0, 1900.0, 0.45)]:
        s0, phase = int(start * SR), 0.0
        for i in range(s0, len(buf)):
            t = (i - s0) / SR
            f = f0 + (f1 - f0) * (1.0 - math.exp(-t * 28.0))  # the pitch rises as it closes
            phase += TAU * f / SR
            buf[i] += gain * attack(t, 0.002) * math.exp(-t * 22.0) * math.sin(phase)
    return buf


def harp():
    buf = [0.0] * int(SR * 1.4)
    rng = random.Random(11)
    for start, f in [(0.0, 392.0), (0.09, 523.25), (0.18, 659.25), (0.27, 783.99)]:  # G4 C5 E5 G5
        n = int(round(SR / f))  # Karplus-Strong plucked string
        line = [rng.uniform(-1, 1) for _ in range(n)]
        s0 = int(start * SR)
        for i in range(s0, len(buf)):
            k = (i - s0) % n
            v = line[k]
            line[k] = 0.996 * 0.5 * (v + line[(k + 1) % n])
            buf[i] += 0.6 * v
    return buf


def write(name, buf):
    fade = int(0.02 * SR)  # no click at the end
    for i in range(fade):
        buf[len(buf) - 1 - i] *= i / fade
    peak = max(abs(x) for x in buf) or 1.0
    path = os.path.join(outdir, name)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(b"".join(struct.pack("<h", int(32767 * 0.8 * x / peak)) for x in buf))
    print(f"wrote {path} ({len(buf) / SR:.2f}s, {SR} Hz mono 16-bit)")


for name, make in [("bell.wav", bell), ("marimba.wav", marimba), ("glass.wav", glass),
                   ("knock.wav", knock), ("drop.wav", drop), ("harp.wav", harp)]:
    write(name, make())

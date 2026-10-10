#!/usr/bin/env python3
"""Generate assets/sounds/sent.wav: an original "swoosh" made for zmail and
dedicated to the public domain (CC0-1.0). Deterministic output.
Usage: tools/make-sent-swoosh.py [out.wav]

Noise swept upward through a resonant band-pass filter, swelling and then
trailing off (~0.42 s, 44.1 kHz, 16-bit mono WAV).
"""
import math
import os
import random
import struct
import sys
import wave

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(__file__), "..", "assets", "sounds", "sent.wav")
sr, dur = 44100, 0.42
n = int(sr * dur)
rng = random.Random(20261010)  # fixed seed: the same file every run
low = band = 0.0
buf = []
for i in range(n):
    x = i / n
    # The filter's centre climbs from 500 Hz to 5 kHz: the "whoosh past".
    fc = 500.0 * (10.0 ** x)
    f = 2.0 * math.sin(math.pi * fc / sr)
    q = 0.22
    high = rng.uniform(-1.0, 1.0) - low - q * band
    band += f * high
    low += f * band
    # Swell to a peak two thirds of the way through, then fall away quickly.
    env = math.sin(math.pi * x ** 1.6) ** 2
    buf.append(band * env)
fade = int(0.01 * sr)
for i in range(fade):
    buf[n - 1 - i] *= i / fade
peak = max(abs(v) for v in buf) or 1.0
with wave.open(out, "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(sr)
    w.writeframes(b"".join(struct.pack("<h", int(32767 * 0.6 * v / peak)) for v in buf))
print(f"wrote {out} ({dur}s, {sr} Hz mono 16-bit)")

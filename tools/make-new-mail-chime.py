#!/usr/bin/env python3
"""Generate assets/sounds/new-mail.wav: an original two-note chime made for
zmail and dedicated to the public domain (CC0-1.0). Deterministic output.
Usage: tools/make-new-mail-chime.py [out.wav]"""
import math
import os
import struct
import sys
import wave

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(__file__), "..", "assets", "sounds", "new-mail.wav")
sr, dur = 44100, 0.9
notes = [(0.0, 880.0), (0.14, 1318.51)]  # A5, then E6
n = int(sr * dur)
buf = [0.0] * n
for start, f in notes:
    s0 = int(start * sr)
    for i in range(s0, n):
        t = (i - s0) / sr
        env = math.exp(-t * 5.5) * min(1.0, t / 0.004)
        v = (math.sin(2 * math.pi * f * t)
             + 0.35 * math.sin(2 * math.pi * 2 * f * t) * math.exp(-t * 9)
             + 0.12 * math.sin(2 * math.pi * 3 * f * t) * math.exp(-t * 14))
        buf[i] += 0.32 * env * v
peak = max(abs(x) for x in buf)
with wave.open(out, "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(sr)
    w.writeframes(b"".join(struct.pack("<h", int(32767 * 0.8 * x / peak)) for x in buf))

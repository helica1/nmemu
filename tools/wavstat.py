#!/usr/bin/env python3
"""wavstat.py <file.wav>...  - per-second RMS (dBFS) of channel 0, to tell bursts from sustained sound."""
import sys, wave, struct, math
for f in sys.argv[1:]:
    w = wave.open(f); n = w.getnframes(); ch = w.getnchannels(); sr = w.getframerate(); sw = w.getsampwidth()
    data = w.readframes(n)
    fmt = {2: 'h', 4: 'i'}[sw]
    s = struct.unpack("<%d%s" % (n * ch, fmt), data)[::ch]
    full = float(2 ** (8 * sw - 1))
    out = []
    for t in range(0, n, sr):
        seg = s[t:t + sr]
        r = math.sqrt(sum(x * x for x in seg) / max(1, len(seg))) / full
        out.append("%5.0f" % (20 * math.log10(r) if r > 0 else -99))
    print("%-40s" % f.split('/')[-1], " ".join(out))

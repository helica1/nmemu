#!/usr/bin/env python3
"""pitch.py <file.wav> [start_s] - fundamental of channel 0 by zero crossings over the last second (or from start_s)"""
import sys, wave, struct
w = wave.open(sys.argv[1]); n = w.getnframes(); ch = w.getnchannels(); sr = w.getframerate(); sw = w.getsampwidth()
s = struct.unpack("<%d%s" % (n*ch, {2:'h',4:'i'}[sw]), w.readframes(n))[::ch]
start = int(float(sys.argv[2]) * sr) if len(sys.argv) > 2 else max(0, n - sr)
seg = s[start:]
seg = [x - sum(seg)/len(seg) for x in seg]
ups = [i for i in range(1, len(seg)) if seg[i-1] < 0 <= seg[i]]
print("%.1f Hz" % ((len(ups) - 1) * sr / (ups[-1] - ups[0])) if len(ups) > 2 else "no signal")

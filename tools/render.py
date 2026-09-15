#!/usr/bin/env python3
"""Render Nord Modular .pch patches to WAV files with the emulator console.

  tools/render.py <outdir> <patch.pch>... [--seconds N] [--notes 48,55,60,64] [--out34] [--no-fold34] [--console path]

By default 2Output modules aimed at outputs 3/4 are pointed at 1/2 (a Micro Modular has no 3/4).

Each patch is uploaded 3.5 s after power-on (the OS needs ~3 s to boot), a note sequence is
played from 4.5 s on (one note per second, each held for a second), and the WAV is trimmed to
start at 4.0 s so the boot silence is gone. Peak levels are printed per patch.
"""
import os, subprocess, sys, wave, struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OS_FILE = os.path.join(ROOT, "..", "roms", "MicroModularUpdate303b.exe")
BOOT_FILE = os.path.join(ROOT, "..", "roms", "modular_rack_bootflash_3.03.bin")
SR = 96000
LOAD_AT = 3.5
TRIM_AT = 4.0
DAC_OFFSET = 341

def render(console, pch, outdir, seconds, notes, out34=False, fold34=True):
    name = os.path.splitext(os.path.basename(pch))[0]
    tmp = os.path.join(outdir, name + ".raw.wav")
    out = os.path.join(outdir, name + ".wav")
    log = os.path.join(outdir, name + ".log")
    cmd = [console, "--os", OS_FILE, "--boot", BOOT_FILE, "--dsp", "0", "--seconds", str(seconds)] + (["--fold34"] if fold34 else []) + ["--pch", f"{LOAD_AT}:{pch}", "--wav", tmp]
    if out34:
        cmd.append("--out34")
        out = os.path.join(outdir, name + " (out 3-4).wav")
    for i, n in enumerate(notes):
        cmd += ["--note", f"{4.5 + i:.1f}:{n}"]
    with open(log, "w") as f:
        r = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0 or not os.path.exists(tmp):
        return name, None, "console failed, see " + log
    # trim and measure
    w = wave.open(tmp); nframes = w.getnframes(); raw = w.readframes(nframes); w.close()
    start = int(TRIM_AT * SR)
    frames = raw[start * 4:]
    ww = wave.open(out, "wb"); ww.setnchannels(2); ww.setsampwidth(2); ww.setframerate(SR); ww.writeframes(frames); ww.close()
    os.remove(tmp)
    peak = [0, 0]
    for i in range(0, len(frames) // 4, 8):
        l, r = struct.unpack_from("<hh", frames, i * 4)
        peak[0] = max(peak[0], abs(l)); peak[1] = max(peak[1], abs(r))
    return name, peak, out

def main():
    args = sys.argv[1:]
    seconds = 9.5
    notes = [48, 55, 60, 64]
    out34 = False
    fold34 = True
    console = os.path.join(ROOT, "build", "nmmConsole", "nmmConsole")
    files = []
    outdir = None
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--seconds": seconds = float(args[i + 1]); i += 2
        elif a == "--notes": notes = [int(x) for x in args[i + 1].split(",") if x]; i += 2
        elif a == "--console": console = args[i + 1]; i += 2
        elif a == "--out34": out34 = True; i += 1
        elif a == "--no-fold34": fold34 = False; i += 1
        elif outdir is None: outdir = a; i += 1
        else: files.append(a); i += 1
    if not outdir or not files:
        print(__doc__); return 1
    os.makedirs(outdir, exist_ok=True)
    print(f"{'patch':24s} {'peak L':>6s} {'peak R':>6s}  file")
    for pch in files:
        name, peak, info = render(console, pch, outdir, seconds, notes, out34, fold34)
        if peak is None:
            print(f"{name:24s} {'-':>6s} {'-':>6s}  {info}")
        else:
            level = "silent" if max(peak) < 8 else ("clipping" if max(peak) >= 32767 else "")
            print(f"{name:24s} {peak[0]:6d} {peak[1]:6d}  {os.path.basename(info)} {level}")
    return 0

if __name__ == "__main__":
    sys.exit(main())

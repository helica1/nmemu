#!/usr/bin/env python3
"""Render every .pch under a folder to MP3 (or WAV), a few seconds each, in parallel.

  tools/renderall.py <patchdir> <outdir> [--seconds 9] [--jobs 8] [--wav] [--notes 36,48] [--limit N]

Each patch gets the folder structure of the archive mirrored under <outdir>. Two notes are played,
by default C2 and C3 (two and one octaves below middle C), 2.5 s each at 1 s and 4 s; patches
without a Keyboard module ignore them. The synth needs about 0.8 s to take a patch, so leading
silence is trimmed from every file. Files that already exist are skipped, so the run can be
resumed. A CSV next to the output lists every patch with its peak level and whether it stayed silent.
"""
import csv, os, re, subprocess, sys, tempfile, time
from multiprocessing import Pool

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OS_FILE = os.path.join(ROOT, "..", "roms", "MicroModularUpdate303b.exe")
CONSOLE = os.path.join(ROOT, "build", "nmmConsole", "nmmConsole")
FFMPEG = "ffmpeg"


def render(job):
    src, dst, seconds, notes, wav = job
    if os.path.exists(dst):
        return (src, "exists", -1)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmpwav = tempfile.NamedTemporaryFile(suffix=".wav", delete=False).name
    cmd = [CONSOLE, "--os", OS_FILE, "--dsp", "0", "--seconds", str(seconds), "--fold34", "--wav", tmpwav, "--pch", "0.2:" + src]
    for i, n in enumerate(notes):
        cmd += ["--note", "%.1f:%d:2.5" % (1.0 + i * 3.0, n)]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=600)
    except subprocess.TimeoutExpired:
        return (src, "timeout", -1)
    m = re.search(r"ch0 peak=(\d+).*ch1 peak=(\d+)", r.stdout)
    peak = max(int(m.group(1)), int(m.group(2))) if m else -1
    if "failed to load" in r.stdout or not os.path.exists(tmpwav):
        return (src, "load failed", peak)
    # drop the leading silence (patch load, note-on delay), then encode
    trim = ["-af", "silenceremove=start_periods=1:start_threshold=-70dB:start_silence=0.05"]
    codec = [] if wav else ["-codec:a", "libmp3lame", "-q:a", "4"]
    e = subprocess.run([FFMPEG, "-loglevel", "error", "-y", "-i", tmpwav] + trim + codec + [dst], capture_output=True)
    os.unlink(tmpwav)
    if e.returncode != 0:
        return (src, "encode failed", peak)
    return (src, "silent" if peak < 200 else "ok", peak)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) < 2:
        print(__doc__); return 1
    patchdir, outdir = args[0], args[1]
    opts = sys.argv[1:]
    def opt(name, default):
        return opts[opts.index(name) + 1] if name in opts else default
    seconds = float(opt("--seconds", "9"))
    jobs = int(opt("--jobs", str(max(1, (os.cpu_count() or 4) // 2))))
    notes = [int(x) for x in opt("--notes", "36,48").split(",")]
    limit = int(opt("--limit", "0"))
    wav = "--wav" in opts
    ext = ".wav" if wav else ".mp3"

    files = []
    for dp, dn, fn in os.walk(patchdir):
        for f in sorted(fn):
            if f.lower().endswith(".pch"):
                files.append(os.path.join(dp, f))
    files.sort()
    if limit:
        files = files[:limit]
    jobs_list = [(f, os.path.join(outdir, os.path.splitext(os.path.relpath(f, patchdir))[0] + ext), seconds, notes, wav) for f in files]
    os.makedirs(outdir, exist_ok=True)
    print("%d patches, %d workers, %.0f s each, output %s" % (len(files), jobs, seconds, outdir), flush=True)

    t0 = time.time()
    done = silent = failed = 0
    with open(os.path.join(outdir, "render_log.csv"), "a", newline="") as logf:
        w = csv.writer(logf)
        with Pool(jobs) as pool:
            for n, (src, status, peak) in enumerate(pool.imap_unordered(render, jobs_list, chunksize=4), 1):
                w.writerow([os.path.relpath(src, patchdir), status, peak]); logf.flush()
                if status == "silent": silent += 1
                if status in ("load failed", "encode failed", "timeout"): failed += 1
                done += 1
                if n % 100 == 0 or n == len(jobs_list):
                    el = time.time() - t0
                    print("%6d/%d  silent %d  failed %d  %.0f s elapsed, ~%.0f min left" % (n, len(jobs_list), silent, failed, el, el / n * (len(jobs_list) - n) / 60), flush=True)
    print("done: %d rendered, %d silent, %d failed" % (done, silent, failed))
    return 0


if __name__ == "__main__":
    sys.exit(main())

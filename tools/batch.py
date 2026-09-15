#!/usr/bin/env python3
"""Render a random sample of .pch files and report which stay silent, with the facts that
help explain why (format, unknown modules, DSP load, output routing, synth replies).

  tools/batch.py <patchdir> [--count N] [--seed S] [--csv out.csv] [--seconds 4.5] [--notes 48,60,64,55]
"""
import csv, os, random, re, subprocess, sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OS_FILE = os.path.join(ROOT, "..", "roms", "MicroModularUpdate303b.exe")
CONSOLE = os.path.join(ROOT, "build", "nmmConsole", "nmmConsole")
LOAD_AT = 0.2

NS = {'m': 'http://nmedit.sf.net/ns/ModuleDescriptions'}
MODULES = {}
for m in ET.parse(os.path.join(ROOT, "data", "modules.xml")).getroot().find('m:body', NS).findall('m:module', NS):
    cyc = [a.get('value') for a in m.findall('m:attribute', NS) if a.get('name') == 'cycles']
    MODULES[int(m.get('index'))] = (m.get('name'), float(cyc[0]) if cyc else 0.0)

def analyse_pch(path):
    """static facts from the file"""
    info = {"format": "?", "voices": 1, "load": 0.0, "modules": [], "out_dest": [], "in_out_cabled": False, "has_keyboard": False, "has_audioin": False}
    try:
        text = open(path, "r", errors="replace").read().replace("\r\n", "\n").replace("\r", "\n")
    except OSError:
        return info
    info["format"] = "2.10" if "patch 2.10" in text[:300] else "3.0"
    if info["format"] == "2.10":
        types = [int(x) for x in re.findall(r"^Type=(\d+)", text, re.M)]
        v = re.search(r"^Voices=(\d+)", text, re.M)
        if v: info["voices"] = int(v.group(1))
    else:
        types = []
        for sec in re.findall(r"\[ModuleDump\]\n(.*?)\[/ModuleDump\]", text, re.S):
            for line in sec.strip().splitlines()[1:]:
                p = line.split()
                if len(p) >= 4: types.append(int(p[1]))
        h = re.search(r"\[Header\]\nVersion[^\n]*\n(.*?)\n", text, re.S)
        if h:
            p = h.group(1).split()
            if len(p) >= 8: info["voices"] = int(p[7])
        for sec in re.findall(r"\[ParameterDump\]\n(.*?)\[/ParameterDump\]", text, re.S):
            for line in sec.strip().splitlines()[1:]:
                p = line.split()
                if len(p) >= 6 and p[1] == "4": info["out_dest"].append(int(p[4]))
                if len(p) >= 4 and p[1] == "3": info["out_dest"].append(0)
    info["modules"] = sorted(set(MODULES.get(t, ("?%d" % t, 0))[0] for t in types))
    info["load"] = sum(MODULES.get(t, ("", 0))[1] for t in types)
    info["has_keyboard"] = 1 in types
    info["has_audioin"] = 2 in types
    info["has_output"] = 4 in types or 3 in types or 5 in types
    return info

def run(path, seconds, notes):
    cmd = [CONSOLE, "--os", OS_FILE, "--dsp", "0", "--seconds", str(seconds), "--fold34", "--pch", f"{LOAD_AT}:{path}"]
    for i, n in enumerate(notes):
        cmd += ["--note", f"{0.6 + i * 0.8:.1f}:{n}"]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    out = r.stdout
    res = {"exit": r.returncode, "packets": 0, "acks": 0, "iam": 0, "info": 0, "unknown": 0, "peakL": 0, "peakR": 0, "err": ""}
    m = re.search(r"-> (\d+) packets", out)
    if m: res["packets"] = int(m.group(1))
    m = re.search(r"pc port replies: iam=(\d+) ack=(\d+) info=(\d+)", out)
    if m: res["iam"], res["acks"], res["info"] = int(m.group(1)), int(m.group(2)), int(m.group(3))
    res["unknown"] = len(re.findall(r"unknown module type", out))
    m = re.search(r"ch0 peak=(\d+).*ch1 peak=(\d+)", out)
    if m: res["peakL"], res["peakR"] = int(m.group(1)), int(m.group(2))
    m = re.search(r"failed to load[^\n]*", out)
    if m: res["err"] = m.group(0)
    return res

def main():
    args = sys.argv[1:]
    if not args: print(__doc__); return 1
    patchdir = args[0]; count = 100; seed = 1; csvfile = None; seconds = 4.5; notes = [48, 60, 64, 55]
    i = 1
    while i < len(args):
        if args[i] == "--count": count = int(args[i+1]); i += 2
        elif args[i] == "--seed": seed = int(args[i+1]); i += 2
        elif args[i] == "--csv": csvfile = args[i+1]; i += 2
        elif args[i] == "--seconds": seconds = float(args[i+1]); i += 2
        elif args[i] == "--notes": notes = [int(x) for x in args[i+1].split(",")]; i += 2
        else: i += 1
    files = []
    for dp, dn, fn in os.walk(patchdir):
        for f in fn:
            if f.lower().endswith(".pch"): files.append(os.path.join(dp, f))
    random.seed(seed)
    sample = random.sample(files, min(count, len(files)))
    rows = []
    silent = []
    for n, path in enumerate(sample):
        info = analyse_pch(path)
        res = run(path, seconds, notes)
        peak = max(res["peakL"], res["peakR"])
        row = {"file": os.path.relpath(path, patchdir), "format": info["format"], "voices": info["voices"], "load": round(info["load"], 1),
               "packets": res["packets"], "acks": res["acks"], "iam": res["iam"], "unknown": res["unknown"], "peak": peak,
               "peakL": res["peakL"], "peakR": res["peakR"], "keyboard": info["has_keyboard"], "audioin": info["has_audioin"],
               "output": info["has_output"], "out_dest": " ".join(map(str, info["out_dest"])), "modules": " ".join(info["modules"]), "err": res["err"]}
        rows.append(row)
        flag = "SILENT" if peak < 200 else ""
        print(f"{n+1:3d}/{len(sample)} {flag:6s} peak={peak:6d} acks={res['acks']}/{res['packets']} load={row['load']:6.1f} v={row['voices']:2d} {row['format']} {row['file'][:70]}", flush=True)
        if flag: silent.append(row)
    if csvfile:
        with open(csvfile, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys())); w.writeheader(); w.writerows(rows)
    print(f"\n{len(silent)} of {len(rows)} silent")
    for r in silent:
        print(f"  {r['file'][:60]:60s} fmt={r['format']} v={r['voices']} load={r['load']} acks={r['acks']}/{r['packets']} unk={r['unknown']} out={r['output']} dest={r['out_dest']} kbd={r['keyboard']} in={r['audioin']}")
        print(f"      {r['modules'][:150]}")
    return 0

if __name__ == "__main__":
    sys.exit(main())

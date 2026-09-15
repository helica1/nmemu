#!/usr/bin/env python3
"""rewire.py <in.pch> <out.pch> <module>:<outconn>  - route the given module output straight to the
2Output module's two inputs (3.0 files, poly area). For bisecting silent patches."""
import re, sys
src = open(sys.argv[1], errors="replace").read().replace("\r\n", "\n").replace("\r", "\n")
mod, conn = [int(x) for x in sys.argv[3].split(":")]
# find the 2Output module index in area 1
out_idx = None
for sec in re.findall(r"\[ModuleDump\]\n(.*?)\[/ModuleDump\]", src, re.S):
    lines = sec.strip().splitlines()
    if lines[0].split()[0] != "1": continue
    for line in lines[1:]:
        p = line.split()
        if len(p) >= 4 and p[1] == "4": out_idx = int(p[0])
assert out_idx is not None, "no 2Output module"
def fix(m):
    lines = m.group(1).strip().splitlines()
    if lines[0].split()[0] != "1": return m.group(0)
    keep = [lines[0]]
    for line in lines[1:]:
        p = line.split()
        if len(p) >= 7 and ((int(p[1]) == out_idx and p[3] == "0") or (int(p[4]) == out_idx and p[6] == "0")):
            continue   # drop cables into the output module
        keep.append(line)
    keep.append(f"0 {out_idx} 0 0 {mod} {conn} 1 ")
    keep.append(f"0 {out_idx} 1 0 {mod} {conn} 1 ")
    return "[CableDump]\n" + "\n".join(keep) + "\n[/CableDump]"
dst = re.sub(r"\[CableDump\]\n(.*?)\[/CableDump\]", fix, src, flags=re.S)
open(sys.argv[2], "w").write(dst)

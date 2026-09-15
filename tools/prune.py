#!/usr/bin/env python3
"""prune.py <in.pch> <out.pch> <keep>  - keep only the listed poly-area modules (comma separated indices)
and the cables between them; drop knob/ctrl/morph maps with --nomaps. For bisecting 3.0 patches."""
import re, sys
src = open(sys.argv[1], errors="replace").read().replace("\r\n", "\n").replace("\r", "\n")
keep = set(int(x) for x in sys.argv[3].split(","))
nomaps = "--nomaps" in sys.argv
def area_filter(sec, body, pred):
    lines = body.strip().splitlines()
    if not lines or lines[0].split()[0] != "1": return "[%s]\n%s[/%s]" % (sec, body, sec)
    out = [lines[0]] + [l for l in lines[1:] if pred(l.split())]
    return "[%s]\n%s\n[/%s]" % (sec, "\n".join(out), sec)
def sub(sec, pred):
    global src
    src = re.sub(r"\[%s\]\n(.*?)\[/%s\]" % (sec, sec), lambda m: area_filter(sec, m.group(1), pred), src, flags=re.S)
sub("ModuleDump", lambda p: len(p) >= 4 and int(p[0]) in keep)
sub("CableDump", lambda p: len(p) >= 7 and int(p[1]) in keep and int(p[4]) in keep)
sub("ParameterDump", lambda p: len(p) >= 3 and int(p[0]) in keep)
sub("CustomDump", lambda p: len(p) >= 2 and int(p[0]) in keep)
sub("NameDump", lambda p: len(p) >= 1 and int(p[0]) in keep)
if nomaps:
    for sec in ("KnobMapDump", "CtrlMapDump"):
        src = re.sub(r"\[%s\]\n(.*?)\[/%s\]" % (sec, sec), "[%s]\n[/%s]" % (sec, sec), src, flags=re.S)
    src = re.sub(r"\[MorphMapDump\]\n(.*?)\[/MorphMapDump\]", "[MorphMapDump]\n0 0 0 0 \n[/MorphMapDump]", src, flags=re.S)
open(sys.argv[2], "w").write(src)

#!/usr/bin/env python3
"""pchinfo.py <patch.pch>...  - summarise a 3.0 patch: modules, cables and the parameters that decide
whether it can make a sound on its own (clock on/off, output level/mute, oscillator mutes, inputs)."""
import sys, re, os, subprocess, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkpch
CONSOLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "nmmConsole", "nmmConsole")
NS = mkpch.NS
PARAMS = {}
for m in mkpch.root.find('m:body', NS).findall('m:module', NS):
    PARAMS[int(m.get('index'))] = [(p.get('name'), p.get('role', '')) for p in m.findall('m:parameter', NS) if p.get('class', 'parameter') == 'parameter']

def info(path, cables=True):
    t = open(path, errors='replace').read().replace('\r\n', '\n').replace('\r', '\n')
    if 'patch 3.0' not in t[:400]:
        tmp = tempfile.NamedTemporaryFile(suffix=".pch", delete=False).name
        subprocess.run([CONSOLE, "--convert", path, tmp], capture_output=True)
        t = open(tmp, errors='replace').read(); os.unlink(tmp)
        print("  (legacy file, converted)")
    mods = {}
    for sec in re.findall(r"\[ModuleDump\]\n(.*?)\[/ModuleDump\]", t, re.S):
        ls = sec.strip().splitlines(); area = ls[0].split()[0]
        for l in ls[1:]:
            p = l.split()
            if len(p) >= 4: mods[(area, int(p[0]))] = int(p[1])
    names = {k: "%s%s" % (mkpch.DB.get(v, ('?%d' % v,))[0], k[1]) for k, v in mods.items()}
    h = re.search(r"\[Header\]\nVersion[^\n]*\n(.*?)\n", t, re.S)
    print("  header:", h.group(1).strip() if h else "?")
    print("  modules:", " ".join(sorted(set(names.values()))))
    if cables:
        for sec in re.findall(r"\[CableDump\]\n(.*?)\[/CableDump\]", t, re.S):
            ls = sec.strip().splitlines(); area = ls[0].split()[0]
            for l in ls[1:]:
                p = l.split()
                if len(p) >= 7:
                    print("    %s: %s.%s%s -> %s.in%s" % (area, names.get((area, int(p[4])), p[4]), 'out' if p[6] == '1' else 'in', p[5], names.get((area, int(p[1])), p[1]), p[2]))
    for sec in re.findall(r"\[ParameterDump\]\n(.*?)\[/ParameterDump\]", t, re.S):
        ls = sec.strip().splitlines(); area = ls[0].split()[0]
        for l in ls[1:]:
            p = l.split()
            if len(p) < 3: continue
            ty = int(p[1]); vals = p[3:]
            pd = PARAMS.get(ty, [])
            notes = []
            for i, (pn, role) in enumerate(pd):
                if i >= len(vals): break
                if 'mute' in role and vals[i] != '0': notes.append("%s=%s" % (pn, vals[i]))
                if 'level' in role: notes.append("%s=%s" % (pn, vals[i]))
                if pn == 'on/off': notes.append("on/off=%s" % vals[i])
                if 'out,assign' in role and vals[i] != '0': notes.append("dest=%s" % vals[i])
            if notes: print("    %s %s: %s" % (area, names.get((area, int(p[0])), p[0]), " ".join(notes)))
    for s in ('KnobMapDump', 'CtrlMapDump', 'MorphMapDump'):
        m = re.search(r"\[%s\]\n(.*?)\[/%s\]" % (s, s), t, re.S)
        if m and m.group(1).strip(): print("   ", s, m.group(1).strip().replace('\n', ' | ')[:200])

if __name__ == "__main__":
    cables = "--cables" in sys.argv
    for f in [a for a in sys.argv[1:] if not a.startswith("--")]:
        print("===", f); info(f, cables)

#!/usr/bin/env python3
"""Write Nord Modular 3.0 .pch test patches from a compact description.

mods: list of (index, type, x, y, [param values in dump order] or None for defaults)
cables: list of (srcModule, srcConn, dstModule, dstConn) - outputs to inputs; colour is looked up
"""
import sys, os, xml.etree.ElementTree as ET

NS = {'m': 'http://nmedit.sf.net/ns/ModuleDescriptions'}
SIG = {'audio': 0, 'control': 1, 'logic': 2, 'master-slave': 3}
root = ET.parse(os.path.join(os.path.dirname(__file__), '..', 'data', 'modules.xml')).getroot()
DB = {}
for m in root.find('m:body', NS).findall('m:module', NS):
    ps = [(int(p.get('index')), int(p.get('defaultValue', '0'))) for p in m.findall('m:parameter', NS) if p.get('class', 'parameter') == 'parameter']
    cs = [(int(p.get('index')), int(p.get('defaultValue', '0'))) for p in m.findall('m:parameter', NS) if p.get('class') == 'custom']
    outs = {int(c.get('index')): SIG.get(c.get('signal'), 0) for c in m.findall('m:connector', NS) if c.get('type') == 'output'}
    DB[int(m.get('index'))] = (m.get('name'), ps, cs, outs)

def write(path, mods, cables, voices=1):
    o = []
    o.append("[Header]\nVersion=Nord Modular patch 3.0\n0 127 0 127 2 0 0 %d 4000 2 1 1 0 0 0 0 1 1 1 1 1 1 1 \n[/Header]" % voices)
    o.append("[ModuleDump]\n1 \n" + "".join("%d %d %d %d \n" % (i, t, x, y) for (i, t, x, y, p) in mods) + "[/ModuleDump]")
    o.append("[ModuleDump]\n0 \n[/ModuleDump]")
    o.append("[CurrentNoteDump]\n64 0 0 64 0 0 \n[/CurrentNoteDump]")
    types = {i: t for (i, t, x, y, p) in mods}
    o.append("[CableDump]\n1 \n" + "".join("%d %d %d 0 %d %d 1 \n" % (DB[types[s]][3][sc], d, dc, s, sc) for (s, sc, d, dc) in cables) + "[/CableDump]")
    o.append("[CableDump]\n0 \n[/CableDump]")
    pd = ""
    for (i, t, x, y, p) in mods:
        ps = DB[t][1]
        if not ps: continue
        vals = p if p is not None else [d for (_, d) in ps]
        pd += "%d %d %d %s \n" % (i, t, len(vals), " ".join(str(v) for v in vals))
    o.append("[ParameterDump]\n1 \n" + pd + "[/ParameterDump]")
    o.append("[ParameterDump]\n0 \n[/ParameterDump]")
    o.append("[KnobMapDump]\n[/KnobMapDump]")
    cd = "".join("%d %d %s \n" % (i, len(DB[t][2]), " ".join(str(d) for (_, d) in DB[t][2])) for (i, t, x, y, p) in mods if DB[t][2])
    o.append("[CustomDump]\n1 \n" + cd + "[/CustomDump]")
    o.append("[CustomDump]\n0 \n[/CustomDump]")
    o.append("[NameDump]\n1 \n" + "".join("%d %s%d\n" % (i, DB[t][0], i) for (i, t, x, y, p) in mods) + "[/NameDump]")
    o.append("[NameDump]\n0 \n[/NameDump]")
    open(path, 'w').write("\n".join(o) + "\n")

if __name__ == '__main__':
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    OSCA, OUT2, MIX8, MIX3, MOSC, SLVA, FBANK, FILTE, AMP, KBD, ADSR = 7, 4, 40, 19, 97, 14, 32, 51, 81, 1, 20
    osc = lambda i, y: (i, OSCA, 0, y, [64, 64, 64, 64, 1, 0, 0, 0, 0, 0])
    # T1 one osc
    write(f"{out}/t1_osc.pch", [osc(1, 0), (2, OUT2, 0, 7, [100, 0, 0])], [(1, 0, 2, 0), (1, 0, 2, 1)])
    # T2 4 oscs into mixer8 (~47%)
    write(f"{out}/t2_4osc.pch", [osc(i, i * 6) for i in range(1, 5)] + [(5, MIX8, 1, 0, None), (6, OUT2, 1, 7, [100, 0, 0])],
          [(i, 0, 5, i - 1) for i in range(1, 5)] + [(5, 0, 6, 0), (5, 0, 6, 1)])
    # T3 8 oscs into mixer8 (~90%)
    write(f"{out}/t3_8osc.pch", [osc(i, i * 6) for i in range(1, 9)] + [(9, MIX8, 1, 0, None), (10, OUT2, 1, 7, [100, 0, 0])],
          [(i, 0, 9, i - 1) for i in range(1, 9)] + [(9, 0, 10, 0), (9, 0, 10, 1)])
    # T4 master osc + slave
    write(f"{out}/t4_slave.pch", [(1, MOSC, 0, 0, None), (2, SLVA, 0, 4, [64, 64, 1, 0, 0]), (3, OUT2, 0, 10, [100, 0, 0])],
          [(1, 0, 2, 0), (2, 0, 3, 0), (2, 0, 3, 1)])
    # T5 osc -> filter bank
    write(f"{out}/t5_fbank.pch", [osc(1, 0), (2, FBANK, 0, 7, None), (3, OUT2, 0, 20, [100, 0, 0])], [(1, 0, 2, 0), (2, 0, 3, 0), (2, 0, 3, 1)])
    # T6 osc -> filter E
    write(f"{out}/t6_filte.pch", [osc(1, 0), (2, FILTE, 0, 7, None), (3, OUT2, 0, 20, [100, 0, 0])], [(1, 0, 2, 2), (2, 0, 3, 0), (2, 0, 3, 1)])
    # T7 osc -> amplifier
    write(f"{out}/t7_amp.pch", [osc(1, 0), (2, AMP, 0, 7, None), (3, OUT2, 0, 12, [100, 0, 0])], [(1, 0, 2, 0), (2, 0, 3, 0), (2, 0, 3, 1)])
    # T8 keyboard + adsr (needs a note)
    write(f"{out}/t8_kbd.pch", [(1, KBD, 0, 0, None), osc(2, 3), (3, ADSR, 0, 10, [0, 10, 40, 100, 30, 0]), (4, OUT2, 0, 16, [100, 0, 0])],
          [(1, 1, 3, 1), (2, 0, 3, 0), (3, 1, 4, 0), (3, 1, 4, 1)], voices=2)
    # T9 4 oscs + mixer3 chain (2 mixers)
    write(f"{out}/t9_mix3.pch", [osc(1, 0), osc(2, 6), (3, MIX3, 1, 0, None), (4, OUT2, 1, 7, [100, 0, 0])], [(1, 0, 3, 0), (2, 0, 3, 1), (3, 0, 4, 0), (3, 0, 4, 1)])
    print("written to", out)

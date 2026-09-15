#!/usr/bin/env python3
"""Nord Modular G1 protocol helpers: build GetPatch requests and decode PatchPacket replies.

Usage:
  nmproto.py requests <outdir> <pid>        write getpatch_<section>.syx files
  nmproto.py decode <console log file>      reassemble PC-port sysex from a console run and decode
"""
import sys, os, re

def frame(cc, slot, payload, checksum=True):
    msg = bytearray([0xf0, 0x33, ((cc & 0x1f) << 2) | (slot & 3), 0x06]) + bytearray(payload)
    if checksum: msg.append(sum(msg) & 0x7f)
    msg.append(0xf7)
    return bytes(msg)

# GetPatch sections: (name, sc, payload or None)
SECTIONS = [
    ("header", 0x20, 0x28), ("polymodule", 0x4b, 1), ("commonmodule", 0x4b, 0),
    ("polycable", 0x53, 1), ("commoncable", 0x53, 0), ("polyparam", 0x4c, 1), ("commonparam", 0x4c, 0),
    ("morphmap", 0x66, None), ("knobmap", 0x63, None), ("controlmap", 0x61, None),
    ("polynames", 0x4e, 1), ("commonnames", 0x4e, 0), ("note", 0x68, None),
]

def getpatch(pid, sc, extra):
    p = [pid & 0x7f, sc]
    if extra is not None: p.append(extra)
    return frame(0x17, 0, p)

class Bits:
    def __init__(self, data7):
        # 7-bit MIDI bytes -> bit list (each byte contributes 7 bits, MSB first)
        self.bits = []
        for b in data7:
            for i in range(6, -1, -1): self.bits.append((b >> i) & 1)
        self.pos = 0
    def read(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | (self.bits[self.pos] if self.pos < len(self.bits) else 0); self.pos += 1
        return v
    def remaining(self): return len(self.bits) - self.pos
    def align(self):
        while self.pos % 8: self.pos += 1
    def string16(self):
        s = ""
        for _ in range(16):
            c = self.read(8)
            if c == 0: break
            s += chr(c)
        return s

def decode_section(bits):
    t = bits.read(8)
    if t == 74:      # ModuleDump
        section = bits.read(1); n = bits.read(7)
        mods = [(bits.read(7), bits.read(7), bits.read(7), bits.read(7)) for _ in range(n)]
        return f"ModuleDump section={section} n={n} (type,index,x,y)={mods}"
    if t == 82:      # CableDump
        section = bits.read(1); n = bits.read(15)
        cab = [(bits.read(3), bits.read(7), bits.read(6), bits.read(1), bits.read(7), bits.read(6)) for _ in range(n)]
        return f"CableDump section={section} n={n} (color,src,srcconn,srcIsOut,dst,dstconn)={cab}"
    if t == 77:      # ParameterDump: raw bits since widths depend on module types
        section = bits.read(1); n = bits.read(7)
        raw = []
        while bits.remaining() >= 7:
            raw.append(bits.read(7))
        return f"ParameterDump section={section} nmodules={n} raw7={[hex(v) for v in raw[:40]]}"
    if t == 33:
        f = [bits.read(7), bits.read(7), bits.read(7), bits.read(7), bits.read(5), bits.read(7), bits.read(1), bits.read(1), bits.read(5)+1, bits.read(2), bits.read(12), bits.read(3)]
        return f"Header keymin,keymax,velmin,velmax,bend,portatime,porta,pedal,voices,unk2,separator,octshift={f}"
    if t == 39 or t == 55:
        if t == 55: bits.read(24)
        return f"PatchName type={t} name='{bits.string16()}'"
    if t == 90:
        section = bits.read(1); n = bits.read(7)
        names = [(bits.read(8), bits.string16()) for _ in range(n)]
        return f"NameDump section={section} n={n} {names}"
    return f"section type {t} (raw follows, {bits.remaining()} bits)"

def decode_packet(msg):
    # msg: full sysex bytes
    if len(msg) < 6 or msg[0] != 0xf0 or msg[1] != 0x33: return None
    cc = (msg[2] >> 2) & 0x1f; slot = msg[2] & 3
    payload = msg[4:-1]
    if cc in (0x1c, 0x1d, 0x1e, 0x1f):
        # payload: [0:1 command:1 pid:6] data... checksum
        hdr = payload[0]; data7 = payload[1:-1]
        bits = Bits(data7)
        out = [f"PatchPacket cc={cc:#x} first={cc&1} last={(cc>>1)&1} cmd={(hdr>>6)&1} pid={hdr&0x3f} ({len(data7)} data bytes)"]
        out.append("  " + decode_section(bits))
        return "\n".join(out)
    if cc == 0x16:
        return f"ACK pid1={payload[0]} type={payload[1]:#x} pid2={payload[2]} rest={[hex(b) for b in payload[3:]]}"
    if cc == 0x00:
        return f"IAm sender={payload[0]} v{payload[1]}.{payload[2]:02d} rest={[hex(b) for b in payload[3:]]}"
    if cc == 0x14:
        return f"NMInfo pid={payload[0]} sc={payload[1]:#x} data={[hex(b) for b in payload[2:-1]]}"
    return f"cc={cc:#x} slot={slot} payload={[hex(b) for b in payload]}"

def reassemble(logfile):
    stream = bytearray()
    for line in open(logfile, errors="replace"):
        m = re.match(r"PC   out @([0-9.]+)s?:((?: [0-9a-f]{2})+)", line)
        if m:
            stream += bytes(int(x, 16) for x in m.group(2).split())
    msgs = []
    cur = None
    for b in stream:
        if b == 0xf0: cur = bytearray([b])
        elif cur is not None:
            cur.append(b)
            if b == 0xf7:
                msgs.append(bytes(cur)); cur = None
    return msgs

if __name__ == "__main__":
    if sys.argv[1] == "requests":
        outdir, pid = sys.argv[2], int(sys.argv[3])
        os.makedirs(outdir, exist_ok=True)
        for name, sc, extra in SECTIONS:
            with open(os.path.join(outdir, f"getpatch_{name}.syx"), "wb") as f: f.write(getpatch(pid, sc, extra))
        print("wrote", len(SECTIONS), "requests to", outdir)
    elif sys.argv[1] == "decode":
        for m in reassemble(sys.argv[2]):
            d = decode_packet(m)
            print(m.hex(" ")[:60] + (" ..." if len(m) > 20 else ""))
            if d: print("  ->", d)

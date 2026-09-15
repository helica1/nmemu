#!/usr/bin/env python3
"""Build Nord Modular G1 SysEx test messages: an IAm handshake and a minimal patch upload.

Protocol details follow the Animatek-NME editor (GPL), which follows the original nmedit
libnmprotocol. Wire format: F0 33 [0:1 cc:5 slot:2] 06 [payload] [checksum] F7, checksum =
sum of all bytes from F0 through the payload, mod 128.
"""
import sys, os

class BitWriter:
    def __init__(self): self.bits = []
    def write(self, value, n):
        for i in range(n - 1, -1, -1): self.bits.append((value >> i) & 1)
    def string16(self, s):
        s = s[:16]
        for ch in s: self.write(ord(ch), 8)
        if len(s) < 16: self.write(0, 8)
    def align(self):
        while len(self.bits) % 8: self.bits.append(0)
    def bytes(self):
        self.align()
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            b = 0
            for j in range(8): b = (b << 1) | self.bits[i + j]
            out.append(b)
        return bytes(out)

def pack7(raw):
    out = bytearray(); buf = 0; held = 0
    for b in raw:
        buf = (buf << 8) | b; held += 8
        while held >= 7:
            held -= 7; out.append((buf >> held) & 0x7f)
    if held > 0: out.append((buf << (7 - held)) & 0x7f)
    return bytes(out)

def frame(cc, slot, payload, checksum=True):
    msg = bytearray([0xf0, 0x33, ((cc & 0x1f) << 2) | (slot & 3), 0x06]) + bytearray(payload)
    if checksum: msg.append(sum(msg) & 0x7f)
    msg.append(0xf7)
    return bytes(msg)

def iam():
    return frame(0x00, 0, [0x00, 0x03, 0x03], checksum=False)

# ---- patch sections (raw 8 bit) ----
# module: (type, index, x, y, name, params(list of (value,bits)), customs(list of values))
OSC_A = 7
OUT2 = 4

def sec_patchname(name):
    w = BitWriter(); w.write(55, 8); w.write(0, 8); w.write(0, 8); w.write(0, 8); w.string16(name); return w.bytes()

def sec_header(voices=1):
    w = BitWriter(); w.write(33, 8)
    w.write(0, 7); w.write(127, 7); w.write(0, 7); w.write(127, 7)   # key/vel ranges
    w.write(2, 5)            # bend range
    w.write(0, 7)            # portamento time
    w.write(0, 1)            # portamento
    w.write(1, 1)            # pedal mode
    w.write(voices - 1, 5)   # voices, 0 based
    w.write(0, 2)            # unknown2
    w.write(4000, 12)        # separator position
    w.write(2, 3)            # octave shift
    for _ in range(7): w.write(1, 1)   # cable visibility
    w.write(0, 1); w.write(0, 1)       # voice retrigger common/poly
    w.write(0xf, 4); w.write(0, 3)
    return w.bytes()

def sec_moduledump(section, modules):
    w = BitWriter(); w.write(74, 8); w.write(section, 1); w.write(len(modules), 7)
    for (t, idx, x, y, name, params, customs) in modules:
        w.write(t, 7); w.write(idx, 7); w.write(x, 7); w.write(y, 7)
    return w.bytes()

def sec_notedump():
    w = BitWriter(); w.write(105, 8)
    w.write(64, 7); w.write(0, 7); w.write(0, 7); w.write(0, 5); w.write(64, 7); w.write(0, 7); w.write(0, 7)
    return w.bytes()

def sec_cabledump(section, cables):
    # cable: (color, src, srcConn, srcIsOutput, dst, dstConn)
    w = BitWriter(); w.write(82, 8); w.write(section, 1); w.write(len(cables), 15)
    for (color, src, sc, so, dst, dc) in cables:
        w.write(color, 3); w.write(src, 7); w.write(sc, 6); w.write(so, 1); w.write(dst, 7); w.write(dc, 6)
    return w.bytes()

def sec_paramdump(section, modules):
    w = BitWriter(); w.write(77, 8); w.write(section, 1)
    withparams = [m for m in modules if m[5]]
    w.write(len(withparams), 7)
    for (t, idx, x, y, name, params, customs) in withparams:
        w.write(idx, 7); w.write(t, 7)
        for (v, bits) in params: w.write(v, bits)
    return w.bytes()

def sec_morphmap():
    w = BitWriter(); w.write(101, 8)
    for _ in range(4): w.write(0, 7)
    for _ in range(4): w.write(0, 2)
    w.write(0, 5)
    return w.bytes()

def sec_knobmap():
    w = BitWriter(); w.write(98, 8)
    for _ in range(23): w.write(0, 1)
    return w.bytes()

def sec_controlmap():
    w = BitWriter(); w.write(96, 8); w.write(0, 7); return w.bytes()

def sec_customdump(section, modules):
    w = BitWriter(); w.write(91, 8); w.write(section, 1)
    withcustom = [m for m in modules if m[6]]
    w.write(len(withcustom), 7)
    for (t, idx, x, y, name, params, customs) in withcustom:
        w.write(idx, 7); w.write(len(customs), 8)
        for v in customs: w.write(v, 8)
    return w.bytes()

def sec_namedump(section, modules):
    w = BitWriter(); w.write(90, 8); w.write(section, 1); w.write(len(modules), 7)
    for (t, idx, x, y, name, params, customs) in modules:
        w.write(idx, 8); w.string16(name)
    return w.bytes()

def build_patch(name, poly, common, poly_cables, common_cables, voices=1):
    return [
        sec_patchname(name),
        sec_header(voices),
        sec_moduledump(1, poly), sec_moduledump(0, common),
        sec_notedump(),
        sec_cabledump(1, poly_cables), sec_cabledump(0, common_cables),
        sec_paramdump(1, poly), sec_paramdump(0, common),
        sec_morphmap(), sec_knobmap(), sec_controlmap(),
        sec_customdump(1, poly), sec_customdump(0, common),
        sec_namedump(1, poly), sec_namedump(0, common),
    ]

def packetize(sections, slot=0, packet_bytes=166):
    stream = []  # list of (byte, sectionsEndedHere)
    packets = []
    cur = bytearray(); ended = 0
    for s in sections:
        for b in s:
            if len(cur) == packet_bytes:
                packets.append((bytes(cur), ended)); cur = bytearray(); ended = 0
            cur.append(b)
        ended += 1
    if cur: packets.append((bytes(cur), ended))
    frames = []
    for i, (data, ended) in enumerate(packets):
        first = i == 0; last = i == len(packets) - 1
        cc = 0x1c | (1 if first else 0) | (2 if last else 0)
        payload = bytes([0x40 | (ended & 0x3f)]) + pack7(data)
        frames.append(frame(cc, slot, payload))
    return frames

def osc_to_out(section=0):
    # OscA: freq coarse, fine, kbt, pw, waveform(2 bits), pm1, pm2, fma, pwm, mute(1 bit)
    osc = (OSC_A, 1, 0, 0, "OscA", [(64,7),(64,7),(64,7),(64,7),(1,2),(0,7),(0,7),(0,7),(0,7),(0,1)], [0])
    out = (OUT2, 2, 0, 7, "2Output", [(100,7),(0,2),(0,1)], [])
    cables = [(0, 1, 0, 1, 2, 0), (0, 1, 0, 1, 2, 1)]  # osc out -> left, right (audio = 0)
    mods = [osc, out]
    if section == 0:
        return build_patch("emutest", [], mods, [], cables)
    return build_patch("emutest", mods, [], cables, [])

if __name__ == "__main__":
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    section = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, "iam.syx"), "wb") as f: f.write(iam())
    secs = osc_to_out(section)
    total = sum(len(s) for s in secs)
    frames = packetize(secs)
    for i, fr in enumerate(frames):
        with open(os.path.join(outdir, f"patch{i}.syx"), "wb") as f: f.write(fr)
    print(f"sections: {[len(s) for s in secs]} total {total} bytes -> {len(frames)} packet(s)")
    for i, fr in enumerate(frames): print(f"packet {i}: {len(fr)} bytes: {fr[:12].hex(' ')} ... {fr[-3:].hex(' ')}")
    print("iam:", iam().hex(' '))

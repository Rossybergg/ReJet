"""Disassemble NV2A vertex programs dumped by the loader (REJET_DUMPVS=1 -> vsh/NNN.bin).

Encoding (128-bit instructions, dword 0 unused), as documented for the NV2A GPU:
  dword1: ILU op [25..27], MAC op [21..24], const index [13..20], input v [9..12],
          A neg [8], A swizzle xyzw [6,4,2,0]
  dword2: A temp [28..31], A mux [26..27], B neg [25], B swizzle [23,21,19,17], B temp [13..16],
          B mux [11..12], C neg [10], C swizzle [8,6,4,2], C temp high [0..1]
  dword3: C temp low [30..31], C mux [28..29], MAC temp mask [24..27], temp [20..23],
          ILU temp mask [16..19], output mask [12..15], output is register (1) / constant (0) [11],
          output address [3..10], output from ILU (1) / MAC (0) [2], a0.x relative [1], final [0]
  mux: 1 = temp r, 2 = input v, 3 = constant c. Constants c[0..191]; D3D's c0 is c[96].

    python tools/loader/vsh_decode.py build/loader/vsh/106.bin [more.bin ...]
"""
import struct
import sys
from pathlib import Path

MAC = ["nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4", "dst", "min", "max", "slt", "sge", "arl"]
ILU = ["nop", "mov", "rcp", "rcc", "rsq", "exp", "log", "lit"]
OUTS = {0: "oPos", 3: "oD0", 4: "oD1", 5: "oFog", 6: "oPts", 7: "oB0", 8: "oB1",
        9: "oT0", 10: "oT1", 11: "oT2", 12: "oT3"}


def field(w, shift, bits):
    return (w >> shift) & ((1 << bits) - 1)


def swz(w, shifts):
    s = "".join("xyzw"[field(w, sh, 2)] for sh in shifts)
    return "" if s == "xyzw" else "." + (s[0] if len(set(s)) == 1 else s)


def mask(m):
    return "" if m == 0xF else "." + "".join(c for c, b in zip("xyzw", (8, 4, 2, 1)) if m & b)


def src(name, mux, reg, v, const, a0x, neg, sw):
    if mux == 1:
        r = f"r{reg}" if reg != 12 else "oPos"
    elif mux == 2:
        r = f"v{v}"
    elif mux == 3:
        r = f"c[a0.x+{const - 96}]" if a0x else f"c{const - 96}"
    else:
        r = f"?mux{mux}"
    return ("-" if neg else "") + r + sw


def decode(words):
    w1, w2, w3 = words[1], words[2], words[3]
    ilu, mac = field(w1, 25, 3), field(w1, 21, 4)
    const, v = field(w1, 13, 8), field(w1, 9, 4)
    a0x = field(w3, 1, 1)
    a = src("a", field(w2, 26, 2), field(w2, 28, 4), v, const, a0x, field(w1, 8, 1), swz(w1, (6, 4, 2, 0)))
    b = src("b", field(w2, 11, 2), field(w2, 13, 4), v, const, a0x, field(w2, 25, 1), swz(w2, (23, 21, 19, 17)))
    creg = (field(w2, 0, 2) << 2) | field(w3, 30, 2)
    c = src("c", field(w3, 28, 2), creg, v, const, a0x, field(w2, 10, 1), swz(w2, (8, 6, 4, 2)))
    mac_mask, temp, ilu_mask = field(w3, 24, 4), field(w3, 20, 4), field(w3, 16, 4)
    o_mask, o_is_reg, o_addr, o_from_ilu = field(w3, 12, 4), field(w3, 11, 1), field(w3, 3, 8), field(w3, 2, 1)
    final = field(w3, 0, 1)

    out = []
    mac_args = {"mov": [a], "mul": [a, b], "add": [a, c], "mad": [a, b, c], "arl": [a]}
    if mac:
        name = MAC[mac] if mac < len(MAC) else f"mac{mac}"
        args = mac_args.get(name, [a, b])
        dests = []
        if mac_mask:
            dests.append(("a0.x" if name == "arl" else f"r{temp}") + ("" if name == "arl" else mask(mac_mask)))
        if o_mask and not o_from_ilu:
            dests.append((OUTS.get(o_addr, f"o{o_addr}") if o_is_reg else f"c{o_addr - 96}") + mask(o_mask))
        out.append(f"{name} {', '.join(dests) or '-'}, {', '.join(args)}")
    if ilu:
        name = ILU[ilu]
        dests = []
        if ilu_mask:
            dests.append(f"r{1 if mac else temp}{mask(ilu_mask)}")  # paired with MAC: ILU writes r1
        if o_mask and o_from_ilu:
            dests.append((OUTS.get(o_addr, f"o{o_addr}") if o_is_reg else f"c{o_addr - 96}") + mask(o_mask))
        out.append(f"{name} {', '.join(dests) or '-'}, {c}")
    return out or ["nop"], final


def main(paths):
    for p in paths:
        d = Path(p).read_bytes()
        header = struct.unpack_from("<I", d, 0)[0]
        n = header >> 16
        print(f"== {p}: header {header:08X}, {n} instructions")
        for i in range(n):
            words = struct.unpack_from("<4I", d, 4 + 16 * i)
            text, final = decode(words)
            print(f"  {i:3}: " + " : ".join(text) + ("   ; final" if final else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

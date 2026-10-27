"""Infer the calling convention of every XDK entry point in one XBE section that is called
from outside the XDK (game, engine or ADX code).

For each such function, report:
  - stdcall argument bytes (from `ret N`; a function that starts with `jmp` is followed to its
    target, as XDK 4134 has many jmp stubs),
  - whether ecx/edx/eax are read before being written (register arguments: fastcall or
    XDK custom conventions),
  - call sites and name.
Writes config/<section>_abi.csv (for example config/d3d_abi.csv, config/dsound_abi.csv). Used
to write the HLE prototypes in loader/core/.

    python tools/loader/xdk_abi.py D3D
    python tools/loader/xdk_abi.py DSOUND
"""
import argparse
import csv
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

import capstone
from capstone import x86

ROOT = Path(__file__).resolve().parents[2]
XBE = ROOT / "orig" / "Default.xbe"

REGS = {x86.X86_REG_EAX: "eax", x86.X86_REG_ECX: "ecx", x86.X86_REG_EDX: "edx",
        x86.X86_REG_AX: "eax", x86.X86_REG_AL: "eax", x86.X86_REG_CX: "ecx", x86.X86_REG_CL: "ecx",
        x86.X86_REG_DX: "edx", x86.X86_REG_DL: "edx"}


def sections(d):
    """{name: (va, raw, raw_size)} from the XBE header."""
    base = struct.unpack_from("<I", d, 0x104)[0]
    n, sh = struct.unpack_from("<II", d, 0x11C)
    out = {}
    for i in range(n):
        _, va, _, raw, rs, name_va = struct.unpack_from("<6I", d, sh - base + i * 0x38)
        out[d[name_va - base:d.index(b"\0", name_va - base)].decode()] = (va, raw, rs)
    return out


class Image:
    def __init__(self, d):
        self.d = d
        self.secs = sections(d)

    def code(self, va, size):
        for sva, raw, rs in self.secs.values():
            if sva <= va < sva + rs:
                n = min(size, sva + rs - va)
                return self.d[raw + va - sva: raw + va - sva + n]
        return b""


def analyse(img, md, funcs, addr, depth=0):
    end = min((f for f in funcs if f > addr), default=addr + 0x1000)
    read_first, written, rets = set(), set(), Counter()
    stack_args = 0
    for k, ins in enumerate(md.disasm(img.code(addr, end - addr), addr)):
        if k == 0 and ins.mnemonic == "jmp" and ins.operands and ins.operands[0].type == x86.X86_OP_IMM and depth < 3:
            return analyse(img, md, funcs, ins.operands[0].imm, depth + 1)
        regs_read, regs_written = ins.regs_access()
        for r in regs_read:
            n = REGS.get(r)
            if n and n not in written:
                read_first.add(n)
        for r in regs_written:
            n = REGS.get(r)
            if n:
                written.add(n)
        # xor r, r reads r syntactically but defines it
        if ins.mnemonic == "xor" and len(ins.operands) == 2 and ins.op_str.split(",")[0].strip() == \
                ins.op_str.split(",")[1].strip():
            read_first.discard(REGS.get(ins.operands[0].reg, ""))
        for op in ins.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_ESP and op.mem.disp > 0:
                stack_args = max(stack_args, op.mem.disp)
        if ins.mnemonic == "ret":
            rets[int(ins.op_str, 16) if ins.op_str else 0] += 1
    return read_first, rets, stack_args


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("section", help="XBE section, e.g. D3D or DSOUND")
    args = ap.parse_args()
    d = XBE.read_bytes()
    img = Image(d)
    if args.section not in img.secs:
        sys.exit(f"no section {args.section}; have {', '.join(img.secs)}")
    sva, _, srs = img.secs[args.section]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    origin, names = {}, {}
    with open(ROOT / "config" / "functions.csv", newline="") as f:
        for r in csv.DictReader(f):
            a = int(r["address"], 16)
            origin[a] = r["origin"]
            names[a] = r["name"]
    funcs = sorted(names)
    sites = defaultdict(int)
    with open(ROOT / "build" / "calls_raw.csv", newline="") as f:
        for r in csv.DictReader(f):
            if r["callee_section"] == args.section and r["caller"] and origin.get(int(r["caller"], 16)) != "xdk":
                sites[int(r["callee"], 16)] += 1

    rows = []
    for a in sorted(sites):
        read_first, rets, stack = analyse(img, md, funcs, a)
        rows.append([f"0x{a:08x}", names.get(a, ""), sites[a], "+".join(sorted(read_first)),
                     ";".join(str(k) for k in sorted(rets)), hex(stack)])
    out = ROOT / "config" / f"{args.section.lower()}_abi.csv"
    with open(out, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["address", "name", "call_sites", "regs_read_before_write", "ret_bytes", "max_esp_disp"])
        w.writerows(rows)
    for r in rows:
        name = r[1].split("::", 1)[-1]
        print(f"{r[0]} {name:45} sites {r[2]:3} regs {r[3] or '-':12} ret {r[4]:8} esp+{r[5]}")
    print(f"{len(rows)} functions -> {out.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

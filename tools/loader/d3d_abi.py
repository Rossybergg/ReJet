"""Infer the calling convention of every D3D entry point the game calls.

For each function in the D3D section that is called from outside D3D, report:
  - stdcall argument bytes (from `ret N` in the function body),
  - whether ecx/edx/eax are read before being written (register arguments: fastcall or
    XDK custom conventions),
  - call sites and name.
Writes config/d3d_abi.csv. Used to write the HLE prototypes in loader/core/d3d/.

    python tools/loader/d3d_abi.py
"""
import csv
import sys
from collections import Counter, defaultdict
from pathlib import Path

import capstone
from capstone import x86

ROOT = Path(__file__).resolve().parents[2]
XBE = ROOT / "orig" / "Default.xbe"
D3D_RAW, D3D_VA, D3D_SIZE = 0x17E000, 0x18D600, 0xE6B0  # JSRF D3D section (raw size; the rest is BSS)

REGS = {x86.X86_REG_EAX: "eax", x86.X86_REG_ECX: "ecx", x86.X86_REG_EDX: "edx",
        x86.X86_REG_AX: "eax", x86.X86_REG_AL: "eax", x86.X86_REG_CX: "ecx", x86.X86_REG_CL: "ecx",
        x86.X86_REG_DX: "edx", x86.X86_REG_DL: "edx"}


def analyse(d, md, funcs, addr):
    end = min((f for f in funcs if f > addr), default=D3D_VA + D3D_SIZE)
    code = d[D3D_RAW + addr - D3D_VA: D3D_RAW + end - D3D_VA]
    read_first, written, rets = set(), set(), Counter()
    stack_args = 0
    for ins in md.disasm(code, addr):
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
    d = XBE.read_bytes()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    origin = {}
    names = {}
    with open(ROOT / "config" / "functions.csv", newline="") as f:
        for r in csv.DictReader(f):
            a = int(r["address"], 16)
            origin[a] = r["origin"]
            names[a] = r["name"]
    d3d_funcs = sorted(a for a in names if D3D_VA <= a < D3D_VA + D3D_SIZE)
    sites = defaultdict(int)
    with open(ROOT / "build" / "calls_raw.csv", newline="") as f:
        for r in csv.DictReader(f):
            if r["callee_section"] == "D3D" and r["caller"] and origin.get(int(r["caller"], 16)) != "xdk":
                sites[int(r["callee"], 16)] += 1

    rows = []
    for a in sorted(sites):
        read_first, rets, stack = analyse(d, md, d3d_funcs, a)
        regs = "+".join(sorted(read_first - {"eax"} if False else read_first))
        rows.append([f"0x{a:08x}", names.get(a, ""), sites[a], regs,
                     ";".join(str(k) for k in sorted(rets)), hex(stack)])
    with open(ROOT / "config" / "d3d_abi.csv", "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["address", "name", "call_sites", "regs_read_before_write", "ret_bytes", "max_esp_disp"])
        w.writerows(rows)
    for r in rows:
        print(f"{r[0]} {r[1][6:]:45} sites {r[2]:3} regs {r[3] or '-':12} ret {r[4]:8} esp+{r[5]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

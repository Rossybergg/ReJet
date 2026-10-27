"""Build config/functions.csv and the XDK call census from the headless exports.

Inputs (regenerate with `python tools/ghidra/export.py`):
    build/functions_raw.csv   ExportFunctions.java
    build/calls_raw.csv       ExportCalls.java
    config/regions.csv        hand-maintained .text origin boundaries

Outputs:
    config/functions.csv      every function with an origin (game/engine/adx/xdk/crt)
    config/xdk_census.csv     every non-game function game code calls, with call counts
    config/vtables.csv        candidate vtables with slot targets and writer (ctor/dtor) functions
    docs/XDK_CENSUS.md        summary by library (the shim worklist)

    python tools/inventory/build_inventory.py
"""
import csv
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build"
CONFIG = ROOT / "config"

# Non-.text sections are whole libraries.
SECTION_ORIGIN = {
    "XONLINE": "xdk", "XNET": "xdk", "D3D": "xdk", "XGRPH": "xdk", "DSOUND": "xdk",
    "XPP": "xdk", "WMADEC": "wma", "EXTERNAL": "xdk", "MMATRIX": "xdk", "DOLBY": "xdk",
}
KERNEL_ORDINAL_BASE = 0x80000000


def load_regions():
    with open(CONFIG / "regions.csv", newline="") as f:
        return [(int(r["start"], 16), int(r["end"], 16), r["origin"]) for r in csv.DictReader(f)]


def origin_of(addr, section, regions):
    if section.startswith("BINK"):
        return "bink"
    if section in SECTION_ORIGIN:
        return SECTION_ORIGIN[section]
    if section == ".text":
        for lo, hi, o in regions:
            if lo <= addr < hi:
                return o
    return "unknown"


def library_of(name, section, origin):
    """Best-effort library label for a callee, for the census summary."""
    if "::" in name:
        lib = name.split("::", 1)[0]
        return "xboxkrnl" if lib.lower().startswith("xboxkrnl") else lib
    if section == ".text":
        return {"crt": "LIBCMT", "bink": "BINK", "xdk": "XAPILIB", "adx": "CRI ADX", "engine": "engine libs"}.get(origin, origin)
    return "BINK" if section.startswith("BINK") else section


def main():
    regions = load_regions()
    with open(CONFIG / "kernel_imports.csv", newline="") as f:
        ordinals = {int(r["ordinal"]): r["name"] for r in csv.DictReader(f)}

    funcs = {}
    with open(BUILD / "functions_raw.csv", newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            a = int(r["address"], 16)
            r["origin"] = origin_of(a, r["section"], regions)
            funcs[a] = r

    with open(CONFIG / "functions.csv", "w", newline="", encoding="utf-8") as f:
        cols = ["address", "size", "section", "origin", "name", "name_source", "callers", "callees", "thunk"]
        w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore", lineterminator="\n")
        w.writeheader()
        for a in sorted(funcs):
            w.writerow(funcs[a])

    # Vtables: origin = majority origin of the slot targets.
    vtables = []
    vt_path = BUILD / "vtables_raw.csv"
    if vt_path.exists():
        with open(vt_path, newline="", encoding="utf-8") as f:
            for r in csv.DictReader(f):
                slots = r["targets"].split(";")
                o = Counter(funcs[int(s, 16)]["origin"] for s in slots if int(s, 16) in funcs)
                r["origin"] = o.most_common(1)[0][0] if o else "unknown"
                vtables.append(r)
        with open(CONFIG / "vtables.csv", "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=["vtable", "slots", "origin", "writers", "targets"],
                               lineterminator="\n")
            w.writeheader()
            for r in sorted(vtables, key=lambda r: int(r["vtable"], 16)):
                w.writerow(r)

    # Census: calls made from game code into anything that is not game code.
    sites = Counter()
    callers = defaultdict(set)
    meta = {}
    indirect = 0
    kernel = Counter()  # program-wide, for the loader's thunk table
    with open(BUILD / "calls_raw.csv", newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["callee_section"] == "EXTERNAL":
                kernel[r["callee_name"]] += 1
            elif r["callee"] and int(r["callee"], 16) >= KERNEL_ORDINAL_BASE:
                o = int(r["callee"], 16) - KERNEL_ORDINAL_BASE
                kernel["xboxkrnl.exe::" + ordinals.get(o, f"#{o}")] += 1
            if not r["caller"]:
                continue
            caller = funcs.get(int(r["caller"], 16))
            if caller is None or caller["origin"] != "game":
                continue
            if r["kind"] == "indirect":
                indirect += 1
                continue
            sec = r["callee_section"]
            name = r["callee_name"]
            if sec == "EXTERNAL":
                origin, key = "xdk", name
            elif int(r["callee"], 16) >= KERNEL_ORDINAL_BASE:
                # Thunk slot Ghidra left as a raw ordinal: 0x80000000 | ordinal.
                o = int(r["callee"], 16) - KERNEL_ORDINAL_BASE
                name = f"xboxkrnl.exe::{ordinals.get(o, f'#{o}')}"
                origin, key, sec = "xdk", name, "EXTERNAL"
            elif sec not in SECTION_ORIGIN and sec != ".text" and not sec.startswith("BINK"):
                indirect += 1  # call through a function-pointer variable in game data
                continue
            else:
                ca = int(r["callee"], 16)
                origin = origin_of(ca, sec, regions)
                key = r["callee"]
            if origin == "game":
                continue
            name = name or (funcs[int(key, 16)]["name"] if key.startswith("0x") and int(key, 16) in funcs else "")
            sites[key] += 1
            callers[key].add(r["caller"])
            meta[key] = (name, sec, origin, library_of(name, sec, origin))

    rows = sorted(sites, key=lambda k: (meta[k][3], -sites[k], k))
    with open(CONFIG / "xdk_census.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["library", "callee", "name", "section", "origin", "call_sites", "game_callers"])
        for k in rows:
            name, sec, origin, lib = meta[k]
            w.writerow([lib, k if k.startswith("0x") else "", name, sec, origin, sites[k], len(callers[k])])

    by_lib = defaultdict(lambda: [0, 0, 0])  # functions, named, sites
    for k in rows:
        lib = meta[k][3]
        by_lib[lib][0] += 1
        by_lib[lib][1] += 0 if (not meta[k][0] or meta[k][0].startswith("FUN_")) else 1
        by_lib[lib][2] += sites[k]

    origins = Counter(r["origin"] for r in funcs.values())
    lines = [
        "# XDK call census",
        "",
        "Generated by `tools/inventory/build_inventory.py`. Do not edit by hand.",
        "",
        "## Functions by origin",
        "",
        "| Origin | Functions |",
        "|---|---|",
    ]
    lines += [f"| {o} | {n:,} |" for o, n in origins.most_common()]
    lines += [
        f"| **Total** | **{len(funcs):,}** |",
        "",
        f"Vtables (`config/vtables.csv`): {len(vtables)} — "
        + ", ".join(f"{o} {n}" for o, n in Counter(v["origin"] for v in vtables).most_common()) + ".",
        "",
        "## Non-game functions called directly from game code",
        "",
        "| Library | Distinct callees | Named | Call sites |",
        "|---|---|---|---|",
    ]
    for lib, (n, named, s) in sorted(by_lib.items(), key=lambda kv: -kv[1][2]):
        lines.append(f"| {lib} | {n} | {named} | {s} |")
    lines += [
        "",
        f"Indirect calls from game code with no static target (vtables, function pointers, "
        f"COM-style D3D device calls): **{indirect:,}**. These are not in the counts above.",
        "",
        "Full list: `config/xdk_census.csv`.",
        "",
        "## Kernel imports (whole program)",
        "",
        f"{sum(kernel.values()):,} call sites reach {len(kernel)} of the {len(ordinals)} kernel imports "
        "in the XBE thunk table (`config/kernel_imports.csv`; the rest are data exports or unused). "
        "The loader must provide all of them.",
        "",
    ]
    (ROOT / "docs" / "XDK_CENSUS.md").write_text("\n".join(lines), encoding="utf-8")

    print(f"{len(funcs)} functions: " + ", ".join(f"{o}={n}" for o, n in origins.most_common()))
    print(f"{len(rows)} distinct non-game callees, {sum(sites.values())} call sites, {indirect} indirect")
    return 0


if __name__ == "__main__":
    sys.exit(main())

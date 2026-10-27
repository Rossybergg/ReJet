"""Run the rejet headless scripts against the work Ghidra project.

The work project (%USERPROFILE%\\GhidraProjects\\rejet-work) is separate from the
shared project the MCP server locks. It holds a full auto-analysis of orig/Default.xbe
plus FixupFunctions.java.

    python tools/ghidra/export.py --fresh        # (re)import + analyse + fix up (~6 min)
    python tools/ghidra/export.py                # re-export from the work project
    python tools/ghidra/export.py --decompile addrs.txt [--out build/decomp.c]

Exports go to build/functions_raw.csv and build/calls_raw.csv.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HOME = Path(os.environ["USERPROFILE"])
GHIDRA = HOME / "Tools" / "ghidra_12.1.4_PUBLIC"
PROJECT_DIR = HOME / "GhidraProjects"
PROJECT = "rejet-work"
SCRIPTS = Path(__file__).resolve().parent
BUILD = ROOT / "build"


def run(cmd, log):
    with open(log, "w", encoding="utf-8") as f:
        rc = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT)
    text = log.read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        if any(k in line for k in ("exported ", "decompiled ", "fixup:", "round ", "ERROR", "LockException")):
            print(line.strip())
    return rc, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fresh", action="store_true", help="re-import and analyse into the work project")
    ap.add_argument("--decompile", metavar="ADDRS", help="decompile the functions containing these addresses")
    ap.add_argument("--out", default=str(BUILD / "decomp.c"), help="output for --decompile")
    args = ap.parse_args()

    BUILD.mkdir(exist_ok=True)
    headless = str(GHIDRA / "support" / "analyzeHeadless.bat")
    base = [headless, str(PROJECT_DIR), PROJECT]
    scripts = ["-scriptPath", str(SCRIPTS)]

    if args.decompile:
        cmd = base + ["-process", "Default.xbe", "-noanalysis", "-readOnly"] + scripts + [
            "-postScript", "DecompileList.java", str(Path(args.decompile).resolve()), str(Path(args.out).resolve())]
        rc, text = run(cmd, BUILD / "ghidra_decompile.log")
        return 0 if rc == 0 and "decompiled " in text else 1

    exports = ["-postScript", "ExportThunks.java", "0x001c4a20", str(BUILD / "thunks_raw.csv"),
               "-postScript", "ExportVtables.java", str(BUILD / "vtables_raw.csv"),
               "-postScript", "ExportFunctions.java", str(BUILD / "functions_raw.csv"),
               "-postScript", "ExportCalls.java", str(BUILD / "calls_raw.csv")]
    if args.fresh:
        cmd = base + ["-import", str(ROOT / "orig" / "Default.xbe"), "-overwrite"] + scripts + [
            "-postScript", "FixupFunctions.java"] + exports
    else:
        cmd = base + ["-process", "Default.xbe", "-noanalysis", "-readOnly"] + scripts + exports

    rc, text = run(cmd, BUILD / "ghidra_export.log")
    if rc != 0 or "exported " not in text:
        print(f"headless run failed (rc={rc}); see {BUILD / 'ghidra_export.log'}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

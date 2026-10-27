"""Check the ReJet workstation: XBE hash, pinned tools, period compiler.

Usage: python tools/doctor.py
Tool folder defaults to %USERPROFILE%\\Tools; override with REJET_TOOLS.
"""
import glob
import hashlib
import importlib.util
import os
import shutil
import subprocess
import sys
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.environ.get("REJET_TOOLS", os.path.join(os.path.expanduser("~"), "Tools"))

results = []


def check(name, ok, detail="", required=True):
    results.append((name, ok, detail, required))


def run(cmd):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        return (p.stdout + p.stderr).strip()
    except (OSError, subprocess.TimeoutExpired):
        return None


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    with open(os.path.join(ROOT, "TOOLS.lock"), "rb") as f:
        lock = tomllib.load(f)

    # Reference binary
    want = open(os.path.join(ROOT, "config", "xbe.sha1")).read().split()[0]
    xbe = os.path.join(ROOT, "orig", "Default.xbe")
    if os.path.exists(xbe):
        got = sha1(xbe)
        check("Default.xbe hash", got == want, got)
    else:
        check("Default.xbe hash", False, "orig/Default.xbe missing (python tools/xiso.py extract <iso> default.xbe orig/Default.xbe)")

    # JDK 21
    javas = glob.glob(r"C:\Program Files\Microsoft\jdk-21*\bin\java.exe")
    if os.environ.get("JAVA_HOME"):
        javas.insert(0, os.path.join(os.environ["JAVA_HOME"], "bin", "java.exe"))
    out = run([javas[0], "-version"]) if javas else None
    check("JDK 21", bool(out) and '"21.' in out, out.splitlines()[0] if out else "not found")

    # Ghidra + ghidra-xbe
    gver = lock["ghidra"]["version"]
    gdir = os.path.join(TOOLS, f"ghidra_{gver}_PUBLIC")
    check(f"Ghidra {gver}", os.path.exists(os.path.join(gdir, "ghidraRun.bat")), gdir)
    ext_dirs = glob.glob(os.path.join(os.environ.get("APPDATA", ""), "ghidra", f"ghidra_{gver}_PUBLIC", "Extensions", "*xbe*"))
    ext_dirs += glob.glob(os.path.join(gdir, "Ghidra", "Extensions", "*xbe*"))
    check("ghidra-xbe extension", bool(ext_dirs), ext_dirs[0] if ext_dirs else "not installed")

    mcp_ext = glob.glob(os.path.join(os.environ.get("APPDATA", ""), "ghidra", f"ghidra_{gver}_PUBLIC", "Extensions", "GhidraMCP"))
    mcp_exe = os.path.join(TOOLS, "ghidra-mcp-venv", "Scripts", "ghidra-mcp.exe")
    check("GhidraMCP extension", bool(mcp_ext), mcp_ext[0] if mcp_ext else "not installed")
    check("ghidra-mcp server", os.path.exists(mcp_exe), mcp_exe)

    # objdiff
    exe = os.path.join(TOOLS, "objdiff", "objdiff-cli-windows-x86_64.exe")
    out = run([exe, "--version"]) if os.path.exists(exe) else None
    check(f"objdiff {lock['objdiff']['version']}", bool(out) and lock["objdiff"]["version"] in out, out or "not found")

    # vgmstream, ffmpeg
    check("vgmstream-cli", os.path.exists(os.path.join(TOOLS, "vgmstream", "vgmstream-cli.exe")), required=False)
    check("ffmpeg", bool(shutil.which("ffmpeg")), shutil.which("ffmpeg") or "not on PATH", required=False)

    # Modern Visual Studio with x86 C++ tools
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", ""), "Microsoft Visual Studio", "Installer", "vswhere.exe")
    vs = run([vswhere, "-latest", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
              "-property", "displayName"]) if os.path.exists(vswhere) else None
    check("Visual Studio C++ (x86)", bool(vs), vs or "not found")

    # Period compiler (user-supplied)
    cls = glob.glob(os.path.join(ROOT, "toolchain", "**", "cl.exe"), recursive=True)
    out = run([cls[0]]) if cls else None
    check("VC++ 7.0 cl 13.00", bool(out) and "13.00" in out,
          out.splitlines()[0] if out else "optional: toolchain/ has no cl.exe", required=False)

    # Python packages for inventory/oracle scripts
    for mod in ("capstone", "pefile"):
        check(f"python: {mod}", importlib.util.find_spec(mod) is not None, required=False)

    failed = 0
    for name, ok, detail, required in results:
        tag = "OK  " if ok else ("FAIL" if required else "warn")
        failed += (not ok and required)
        print(f"[{tag}] {name:<26} {detail}")
    print(f"\n{'all required checks pass' if not failed else f'{failed} required check(s) failing'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

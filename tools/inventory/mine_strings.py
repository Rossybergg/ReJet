"""Mine Default.xbe for source paths and identifiers.

Usage: python tools/inventory/mine_strings.py orig/Default.xbe [out.md]
"""
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from xbeinfo import parse  # noqa: E402

SRC_RE = re.compile(rb"[A-Za-z]:\\[\x20-\x7e]{3,200}?\.(?:cpp|c|h|hpp|inl)", re.I)
IDENT_RE = re.compile(rb"\b(?:Boing|SP_|Sprint)[A-Za-z0-9_]{2,60}")


def section_of(info, raw_off):
    for name, va, vsize, raw, rsize, _ in info["sections"]:
        if raw <= raw_off < raw + rsize:
            return name, va + (raw_off - raw)
    return "?", 0


def main(path, out=None):
    data = open(path, "rb").read()
    info = parse(data)

    sources = {}
    for m in SRC_RE.finditer(data):
        p = m.group().decode("latin-1")
        sources.setdefault(p, section_of(info, m.start()))
    idents = {}
    for m in IDENT_RE.finditer(data):
        s = m.group().decode("latin-1")
        idents.setdefault(s, section_of(info, m.start()))

    lines = [f"# Strings mined from Default.xbe", "",
             f"## Source paths ({len(sources)})", ""]
    for p, (sec, va) in sorted(sources.items(), key=lambda kv: kv[0].lower()):
        lines.append(f"- `{p}` ({sec} {va:#x})")
    lines += ["", f"## Sprint/Boing identifiers ({len(idents)})", ""]
    for s, (sec, va) in sorted(idents.items()):
        lines.append(f"- `{s}` ({sec} {va:#x})")
    text = "\n".join(lines) + "\n"
    if out:
        open(out, "w", encoding="utf-8").write(text)
        print(f"{len(sources)} source paths, {len(idents)} identifiers -> {out}")
    else:
        print(text)


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        print(__doc__)
        sys.exit(2)
    main(*sys.argv[1:])

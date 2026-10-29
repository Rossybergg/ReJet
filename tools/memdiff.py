"""Find a game value from how it changed between F10 markers (REJET_MEMDUMP=1, memdump\\mark<N>.bin).

    python tools/memdiff.py <mark files in order> --steps +n,-1 [--max 99]

Each step is the change between two consecutive marks: "+5" exactly 5 more, "-1" one fewer,
"+" more, "-" fewer, "=" unchanged. Lists the 4-byte, 2-byte and 1-byte values (in memory
present in every mark) that match every step, with their values at each mark. For example, to
find the spray can count: F10, pick up cans, F10, spray once, F10, then --steps +,-1.
"""
import argparse
import struct


def load(path):
    blocks = {}
    with open(path, "rb") as f:
        data = f.read()
    at = 0
    while at + 8 <= len(data):
        addr, n = struct.unpack_from("<II", data, at)
        blocks[addr] = data[at + 8:at + 8 + n]
        at += 8 + n
    return blocks


def matches(seq, steps):
    for a, b, s in zip(seq, seq[1:], steps):
        if s == "=" and b != a or s == "+" and b <= a or s == "-" and b >= a:
            return False
        if s[1:] and b - a != int(s):
            return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("marks", nargs="+")
    ap.add_argument("--steps", required=True, help="one per pair of marks, comma-separated: +5,-1,+,-,=")
    ap.add_argument("--max", type=int, default=99, help="largest plausible value")
    args = ap.parse_args()
    steps = args.steps.split(",")
    if len(steps) != len(args.marks) - 1:
        raise SystemExit("need one step per pair of marks")
    dumps = [load(m) for m in args.marks]
    found = 0
    for addr, first in dumps[0].items():
        others = [d.get(addr) for d in dumps[1:]]
        if any(o is None or len(o) != len(first) for o in others):
            continue
        datas = [first] + others
        for size, fmt in ((4, "<i"), (2, "<h"), (1, "<b")):
            for off in range(0, len(first) - size + 1, size):
                seq = [struct.unpack_from(fmt, d, off)[0] for d in datas]
                if all(0 <= v <= args.max for v in seq) and matches(seq, steps):
                    print(f"{addr + off:08X} ({size} bytes): {' -> '.join(map(str, seq))}")
                    found += 1
                    if found >= 200:
                        print("... more: add a marker step to narrow it down")
                        return
    print(f"{found} candidate(s)")


if __name__ == "__main__":
    main()

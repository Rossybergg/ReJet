"""Summarise a ReJet frame recording (REJET_PERFLOG=1, logs\\perf_*.csv).

    python tools/perf_report.py [perf_*.csv]      (default: the newest under build\\loader\\logs)

Reports the frame-time distribution, every frame that missed its vblank (interval over 1.5
frames) with what took the time, and which F10 marker it was near. Frames with the ReJet menu
open and the first seconds of loading are counted separately: they are not gameplay.
"""
import csv
import glob
import os
import sys

FRAME = 1000.0 / 60.0
TIMED = ["tex", "texup", "vsc", "psc", "draw", "present", "io", "log"]
WAIT = ["late", "vbcb"]  # inside the vblank wait


def load(path):
    with open(path, newline="") as f:
        header = f.readline().strip()
        rows = list(csv.DictReader(f))
    for r in rows:
        for k, v in r.items():
            r[k] = float(v) if "." in v else int(v)
    return header, rows


def pct(values, p):
    v = sorted(values)
    return v[min(len(v) - 1, int(len(v) * p / 100))] if v else 0.0


def cause(r):
    """The biggest contributor to a slow frame."""
    parts = {k: r[k + "_ms"] for k in TIMED}
    shim = sum(v for k, v in parts.items() if k != "present")
    parts["game/other"] = max(0.0, r["work_ms"] - shim)
    parts["vblank wait"] = r["wait_ms"]
    for k in WAIT:
        if k + "_ms" in r:
            parts["wait: " + k] = r[k + "_ms"]
    name = max(parts, key=parts.get)
    return name, parts[name]


def main():
    if len(sys.argv) > 1:
        path = sys.argv[1]
    else:
        files = glob.glob(os.path.join("build", "loader", "logs", "perf_*.csv"))
        if not files:
            sys.exit("no recording found (run with REJET_PERFLOG=1)")
        path = max(files, key=os.path.getmtime)
    header, rows = load(path)
    print(path)
    print(header.lstrip("# "))
    # Each row is written when a frame's work ends; the frame before it was shown after that
    # row's wait. So a frame is on screen at (previous row's t) + (this row's wait), and what the
    # player sees is the gap between those moments ("display"), not the gap between work ends.
    for prev, r in zip(rows, rows[1:]):
        r["shown_ms"] = prev["t_ms"] + r["wait_ms"]
    for prev, r in zip(rows[1:], rows[2:]):
        r["display_ms"] = r["shown_ms"] - prev["shown_ms"]
    rows = rows[2:]
    mark = 0
    for r in rows:
        mark = r["mark"] or mark
        r["near_mark"] = mark
    play = [r for r in rows if not r["menu"] and r["t_ms"] > 5000]
    if not play:
        sys.exit("no gameplay frames")
    iv = [r["display_ms"] for r in play]
    work = [r["work_ms"] for r in play]
    minutes = (play[-1]["t_ms"] - play[0]["t_ms"]) / 60000.0
    slow = [r for r in play if r["display_ms"] > FRAME * 1.5]
    print(f"\n{len(play)} gameplay frames over {minutes:.1f} min ({len(rows) - len(play)} menu/start-up frames left out)")
    print(f"average {1000.0 * len(iv) / sum(iv):.2f} fps")
    print("time on screen per frame, ms: median {:.2f}  p99 {:.2f}  p99.9 {:.2f}  max {:.2f}".format(
        pct(iv, 50), pct(iv, 99), pct(iv, 99.9), max(iv)))
    print("work ms (game + ReJet): median {:.2f}  p99 {:.2f}  max {:.2f}  (budget {:.2f})".format(
        pct(work, 50), pct(work, 99), max(work), FRAME))
    # Frames lost against the 60 Hz clock: a frame that is late but shown before the following
    # vblank costs none (the cadence holds); a stutter costs one or more.
    lost = sum(max(0, round(r["display_ms"] / FRAME) - 1) for r in play)
    loads = sum(1 for r in play if r["display_ms"] > 100)
    print(f"frames lost against 60 Hz: {lost} ({lost / max(minutes, 1e-9):.1f} a minute)")
    print(f"frames on screen for over 1.5 vblanks (stutters): {len(slow)} ({len(slow) / max(minutes, 1e-9):.1f} a minute),"
          f" {loads} of them over 100 ms (loading)")
    by_cause = {}
    for r in slow:
        by_cause.setdefault(cause(r)[0], []).append(r)
    for name, rs in sorted(by_cause.items(), key=lambda kv: -len(kv[1])):
        print(f"  {name}: {len(rs)}")
    print("\nworst frames:")
    for r in sorted(slow, key=lambda r: -r["display_ms"])[:25]:
        name, ms = cause(r)
        extra = " ".join(f"{k} {r[k + '_ms']:.1f}/{r[k + '_n']}" for k in TIMED + WAIT if r.get(k + "_ms", 0) >= 1.0)
        print(f"  frame {r['frame']:>7} t {r['t_ms'] / 1000:7.1f}s  {r['display_ms']:6.1f} ms  "
              f"mark {r['near_mark']:>2}  mostly {name} {ms:.1f} ms  [{extra}]")


if __name__ == "__main__":
    main()

"""The release gate (docs/ALPHA2_PLAN.md, tests/README.md): run before any build goes to testers.

    python tools/release_check.py [--game <folder>] [--no-build] [--update-baseline]
                                  [--setup | --no-setup] [--iso <disc image>]

1. Builds the loader and runs the unit tests (rejet_tests.exe).
2. Runs the game for about three minutes on a scripted route (logos, title, a new game, the
   opening cutscene, skating in the Garage) with fixed settings and REJET_PERFLOG, and fails on:
   - a crash, a fatal fault or a hang (frames stop before the run ends);
   - the route not reaching 3D gameplay;
   - an unimplemented D3D function or render state not in tests/baseline_unimplemented.txt
     (a new code path reached something ReJet ignores);
   - performance worse than this machine's baseline (tests/local/perf_baseline.json, made with
     --update-baseline): stutters, or the CPU time per frame (median, worst 1%).
3. First-run setup, as a new player gets it (a release can play fine on this machine and still
   ship a broken setup): a fresh folder with only ReJet.exe and rejet.dll, the disc image fed
   to the setup screen (REJET_SETUP_IMAGE), and it must extract the game, save the folder in
   the settings and boot it. It runs when the setup path changed since the last release tag
   (SETUP_SOURCES), or with --setup; it extracts the whole disc (~2.5 GB, a few minutes).
Exit code 0 when everything passes. The game is the player's own copy; nothing from it is
written outside build\\ and tests\\local\\ (both git-ignored).
"""
import argparse
import configparser
import csv
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build", "loader")
LOCAL = os.path.join(ROOT, "tests", "local")
BASELINE = os.path.join(LOCAL, "perf_baseline.json")
UNIMPLEMENTED = os.path.join(ROOT, "tests", "baseline_unimplemented.txt")
RUN_SECONDS = 175
FRAME = 1000.0 / 60.0

# Fixed settings, so runs compare: 1080p 16:9 in a window, ReJet's defaults otherwise.
SETTINGS = """[Display]
Width=1920
Height=1080
WindowHeight=720
Aspect=2
VSync=0
Fullscreen=0
Anisotropy=16
ShowFPS=0
[Audio]
Volume=0
MuteInBackground=0
[Console]
Language=1
"""

# Tolerances against the baseline (run-to-run noise on one machine is about 15%).
WORK_MEDIAN_SLACK = 1.25
WORK_P99_SLACK = 1.30
STUTTER_SLACK = 2.0  # per minute: baseline * this + 1


def route():
    """Start on the title, A through the opening, then skate around (0.2 s presses)."""
    ev = [f"{t}:start" for t in (36, 38, 40, 42)]
    t = 45.0
    while t < 90:
        ev.append(f"{t:g}:a")
        t += 1.5
    dirs = ["ls_up"] * 20 + ["ls_left"] * 3 + ["ls_up"] * 10 + ["ls_right"] * 3
    t, i = 90.0, 0
    while t < 170:
        ev.append(f"{t:.1f}:{dirs[i % len(dirs)]}")
        if int(t * 5) % 15 == 0:
            ev.append(f"{t:.1f}:a")
        t += 0.2
        i += 1
    return ",".join(ev)


def find_game(arg):
    if arg:
        return arg
    ini = configparser.ConfigParser()
    ini.read(os.path.join(BUILD, "rejet.ini"))
    folder = ini.get("Game", "Folder", fallback="")
    if folder and os.path.isfile(os.path.join(folder, "default.xbe")):
        return folder
    for cand in [os.path.join(BUILD, "Game")] + sorted(glob.glob(os.path.join(ROOT, "dist", "*", "Game")), reverse=True):
        if os.path.isfile(os.path.join(cand, "default.xbe")):
            return cand
    sys.exit("no game folder found: pass --game <folder with default.xbe>")


def step(name, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {name}{': ' + detail if detail else ''}")
    return ok


def metrics(csv_path):
    with open(csv_path, newline="") as f:
        f.readline()
        rows = [{k: float(v) for k, v in r.items()} for r in csv.DictReader(f)]
    for prev, r in zip(rows, rows[1:]):  # when each frame reached the screen (perf_report.py)
        r["shown"] = prev["t_ms"] + r["wait_ms"]
    for prev, r in zip(rows[1:], rows[2:]):
        r["display"] = r["shown"] - prev["shown"]
    play = [r for r in rows[2:] if not r["menu"] and r["t_ms"] > 5000]
    work = sorted(r["work_ms"] for r in play)
    minutes = (play[-1]["t_ms"] - play[0]["t_ms"]) / 60000.0
    stutters = sum(1 for r in play if FRAME * 1.5 < r["display"] <= 100)  # loading stalls are separate
    return {
        "minutes": round(minutes, 2),
        "last_ms": rows[-1]["t_ms"],
        "max_draws": max(r["draws"] for r in play),
        "work_median_ms": round(work[len(work) // 2], 3),
        "work_p99_ms": round(work[int(len(work) * 0.99)], 3),
        "stutters_per_min": round(stutters / max(minutes, 1e-9), 2),
        "loading_stalls": sum(1 for r in play if r["display"] > 100),
    }


# What the first-run setup touches: a change to any of these since the last release runs step 3.
SETUP_SOURCES = ("loader/core/ui/setup.c", "loader/core/ui/setup.h", "loader/core/iso.c", "loader/core/iso.h",
                 "loader/core/settings.c", "loader/core/settings.h", "loader/core/loader.c")
SETUP_SECONDS = 45  # after setup: long enough for the game's first frames


def setup_changed():
    """Whether SETUP_SOURCES changed since the last release tag (or there is no tag)."""
    tag = subprocess.run(["git", "describe", "--tags", "--abbrev=0", "--match", "v*"], cwd=ROOT,
                         capture_output=True, text=True).stdout.strip()
    if not tag:
        return True
    diff = subprocess.run(["git", "diff", "--name-only", tag, "--", *SETUP_SOURCES], cwd=ROOT,
                          capture_output=True, text=True).stdout
    return bool(diff.strip())


def find_iso(arg):
    if arg:
        return arg
    found = sorted(glob.glob(os.path.join(ROOT, "*.iso")) + glob.glob(os.path.join(ROOT, "*.xiso")))
    return found[0] if found else None


def check_setup(iso):
    """Step 3: a new player's first run, from a folder with only the program files."""
    with tempfile.TemporaryDirectory(prefix="rejet-setup-") as tmp:
        for f in ("ReJet.exe", "rejet.dll"):
            with open(os.path.join(BUILD, f), "rb") as src, open(os.path.join(tmp, f), "wb") as dst:
                dst.write(src.read())
        ini = os.path.join(tmp, "rejet.ini")
        with open(ini, "w") as f:
            f.write(SETTINGS)  # no [Game] Folder: the setup screen must find the game
        env = dict(os.environ, REJET_INI=ini, REJET_SETUP_IMAGE=iso, REJET_RUNSECONDS=str(SETUP_SECONDS),
                   REJET_SKIPWARNING="1")
        print(f"  ....  first-run setup from a fresh folder (extracts the disc; a window opens)")
        r = subprocess.run([os.path.join(tmp, "ReJet.exe")], cwd=tmp, env=env, timeout=1800)
        log = open(os.path.join(tmp, "loader.log"), errors="replace").read() if os.path.isfile(
            os.path.join(tmp, "loader.log")) else ""
        cfg = configparser.ConfigParser()
        cfg.read(ini)
        game = os.path.join(tmp, "Game")
        ok = step("setup extracts the game", "setup: extracted" in log and
                  os.path.isfile(os.path.join(game, "default.xbe")),
                  next((l for l in log.splitlines() if l.startswith("setup: extracted")), "no extraction in the log"))
        ok &= step("setup saves the game folder",
                   os.path.normcase(cfg.get("Game", "Folder", fallback="")) == os.path.normcase(game))
        frames = re.findall(r"d3d: frame (\d+)", log)
        ok &= step("the game boots after setup", r.returncode == 0 and "timeout: stopping" in log and
                   bool(frames), f"up to frame {frames[-1] if frames else 0}, exit code {r.returncode}")
        return ok


def unimplemented(log_text):
    names = set(re.findall(r"d3d: (\w+) not implemented yet", log_text))
    names |= set(re.findall(r"d3d: (SetRenderState_\w+)\([0-9A-F]+\) not applied", log_text))
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", help="folder with the game's default.xbe")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--update-baseline", action="store_true", help="save this run's performance as the baseline")
    ap.add_argument("--setup", action="store_true", help="always test the first-run setup")
    ap.add_argument("--no-setup", action="store_true", help="skip the first-run setup test")
    ap.add_argument("--iso", help="the disc image for the setup test (default: the .iso in the repository root)")
    args = ap.parse_args()
    ok = True

    print("Release check")
    if not args.no_build:
        r = subprocess.run(["cmd", "/c", os.path.join(ROOT, "tools", "loader", "build.bat")], cwd=ROOT,
                           capture_output=True, text=True)
        if not step("build", r.returncode == 0):
            print(r.stdout[-3000:], r.stderr[-2000:])
            return 1
    r = subprocess.run([os.path.join(BUILD, "rejet_tests.exe")], capture_output=True, text=True)
    ok &= step("unit tests", r.returncode == 0, r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "no output")
    if r.returncode:
        print(r.stdout)

    game = find_game(args.game)
    os.makedirs(LOCAL, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        ini = os.path.join(tmp, "rejet.ini")
        with open(ini, "w") as f:
            f.write(SETTINGS)
        env = dict(os.environ, REJET_INI=ini, REJET_PERFLOG="1", REJET_INPUT=route())
        before = set(glob.glob(os.path.join(BUILD, "logs", "perf_*.csv")))
        print(f"  ....  scripted run ({RUN_SECONDS} s; a game window opens)")
        r = subprocess.run([os.path.join(BUILD, "ReJet.exe"), os.path.join(game, "default.xbe"), str(RUN_SECONDS)],
                           cwd=BUILD, env=env, timeout=RUN_SECONDS + 120)
    with open(os.path.join(BUILD, "loader.log"), errors="replace") as f:
        log = f.read()
    fatal = [l for l in log.splitlines() if l.startswith("stop:") or l.startswith("EXCEPTION")]
    ok &= step("no crash", r.returncode == 0 and not fatal and "timeout: stopping" in log,
               fatal[0] if fatal else f"exit code {r.returncode}")
    new_csv = sorted(set(glob.glob(os.path.join(BUILD, "logs", "perf_*.csv"))) - before)
    if not step("frame recording", bool(new_csv)):
        return 1
    m = metrics(new_csv[-1])
    ok &= step("no hang", m["last_ms"] > (RUN_SECONDS - 10) * 1000, f"frames until {m['last_ms'] / 1000:.0f} s")
    ok &= step("reached 3D gameplay", m["max_draws"] >= 150, f"up to {m['max_draws']:.0f} draws a frame")

    with open(UNIMPLEMENTED) as f:
        known = {l.strip() for l in f if l.strip() and not l.startswith("#")}
    new = sorted(unimplemented(log) - known)
    ok &= step("no new unimplemented functions", not new, ", ".join(new))

    print(f"        this run: work median {m['work_median_ms']} ms, worst 1% {m['work_p99_ms']} ms, "
          f"{m['stutters_per_min']} stutters a minute, {m['loading_stalls']} loading stalls")
    if args.update_baseline:
        with open(BASELINE, "w") as f:
            json.dump(m, f, indent=1)
        step("baseline saved", True, os.path.relpath(BASELINE, ROOT))
    elif not os.path.isfile(BASELINE):
        step("performance", True, "no baseline on this machine yet: run with --update-baseline")
    else:
        with open(BASELINE) as f:
            b = json.load(f)
        ok &= step("work per frame (median)", m["work_median_ms"] <= b["work_median_ms"] * WORK_MEDIAN_SLACK,
                   f"{m['work_median_ms']} vs baseline {b['work_median_ms']} ms")
        ok &= step("work per frame (worst 1%)", m["work_p99_ms"] <= b["work_p99_ms"] * WORK_P99_SLACK,
                   f"{m['work_p99_ms']} vs baseline {b['work_p99_ms']} ms")
        ok &= step("stutters", m["stutters_per_min"] <= b["stutters_per_min"] * STUTTER_SLACK + 1,
                   f"{m['stutters_per_min']} vs baseline {b['stutters_per_min']} a minute")
    if not args.no_setup and (args.setup or setup_changed()):
        iso = find_iso(args.iso)
        if iso:
            ok &= check_setup(iso)
        else:
            ok &= step("first-run setup", False, "setup code changed but no disc image found: pass --iso")
    else:
        step("first-run setup", True, "skipped: setup code unchanged since the last release")
    print("RELEASE CHECK PASSED" if ok else "RELEASE CHECK FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

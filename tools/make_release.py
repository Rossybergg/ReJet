"""Build a ReJet release package for testers: dist/ReJet-<version>/ and dist/ReJet-<version>.zip.

    python tools/make_release.py [--allow-dirty] [--skip-check]

The version comes from the VERSION file (semantic: MAJOR.MINOR.PATCH; bump it before every
release). Releases are built from a clean, committed tree so the build string
(<version>+<commit>, shown in the ReJet menu and in every log and capture) identifies the exact
code; --allow-dirty is for trying the packaging only. The package holds ReJet.exe,
rejet.dll, a player readme and the GPL-3.0 licence; never game files (the script refuses anything else).
The release gate (tools/release_check.py: unit tests, a scripted run against this machine's
performance baseline) must pass first; --skip-check is for trying the packaging only.
"""
import hashlib
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "loader"
ALLOWED = {"ReJet.exe", "rejet.dll", "README.txt", "LICENSE.txt"}


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()


def main():
    allow_dirty = "--allow-dirty" in sys.argv
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    # Every version shown (menu, hints, setup, splash, logs, the exe's version resource) is built
    # from VERSION, so a release must bump it: a version already tagged would ship old numbers.
    if git("tag", "--list", f"v{version}") and not allow_dirty:
        sys.exit(f"v{version} is already released: bump VERSION (patch for fixes, minor for features) first.")
    if git("status", "--porcelain", "--untracked-files=no") and not allow_dirty:
        sys.exit("The working tree has uncommitted changes: commit first (or --allow-dirty to test packaging).")

    if "--skip-check" in sys.argv:
        print("Skipping the release check: this package is for trying the packaging, not for testers.")
        subprocess.run(["cmd", "/c", str(ROOT / "tools" / "loader" / "build.bat")], cwd=ROOT, check=True)
    elif subprocess.run([sys.executable, "-I", str(ROOT / "tools" / "release_check.py")], cwd=ROOT).returncode:
        sys.exit("The release check failed: fix it before packaging (tools/release_check.py).")
    build = (ROOT / "loader" / "core" / "version.gen.h").read_text(encoding="utf-8")
    build = build.split('#define REJET_BUILD "')[1].split('"')[0]
    if not build.startswith(version + "+"):
        sys.exit(f"Built {build}, expected version {version}")

    name = f"ReJet-{version}"
    # The package is assembled in a staging folder that holds nothing else. dist/ReJet-<version>/
    # may be someone's test install (game files, settings, saves, captures): only the program
    # files there are refreshed, nothing is deleted.
    stage = ROOT / "dist" / ".stage" / name
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    for f in ("ReJet.exe", "rejet.dll"):
        shutil.copy2(BUILD / f, stage / f)
    readme = (ROOT / "docs" / "release" / "README.txt").read_text(encoding="utf-8")
    (stage / "README.txt").write_text(readme.replace("{version}", build), encoding="utf-8", newline="\r\n")
    licence = (ROOT / "LICENSE").read_text(encoding="utf-8")  # GPL-3.0 travels with the program
    (stage / "LICENSE.txt").write_text(licence, encoding="utf-8", newline="\r\n")

    files = sorted(p.name for p in stage.iterdir())
    if set(files) != ALLOWED:
        sys.exit(f"Unexpected files in the package: {files}")
    archive = ROOT / "dist" / f"{name}.zip"  # not with_suffix: the version has dots
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for f in files:
            z.write(stage / f, f"{name}/{f}")
    install = ROOT / "dist" / name
    install.mkdir(parents=True, exist_ok=True)
    for f in files:
        shutil.copy2(stage / f, install / f)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    print(f"{archive.relative_to(ROOT)}  ({archive.stat().st_size / 1e6:.1f} MB)  build {build}")
    print(f"SHA-256 {digest}")
    print(f"{install.relative_to(ROOT)} updated (other files there kept)")


if __name__ == "__main__":
    main()

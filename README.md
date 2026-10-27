# ReJet

Play **Jet Set Radio Future** (Smilebit / Sega, Xbox, 2002) natively on Windows, from your own disc, with modern enhancements.

> **Status: early work in progress.** The loader boots the game as far as its renderer setup; there is no picture, sound or input yet. See [docs/PROGRESS.md](docs/PROGRESS.md).

ReJet is a **hybrid host**, built the same way as its sister project ReWhacked: a loader maps the original `default.xbe` at its own addresses, implements the Xbox kernel, and replaces the Xbox system libraries (Direct3D 8, DirectSound, controllers) with PC equivalents. The game's own code runs unchanged; enhancements hook specific game functions, each behind a switch.

- **Not affiliated** with Sega, Smilebit, Microsoft or any rights holder. *Jet Set Radio Future* is a trademark of its owners.
- **No game data is included, ever.** You need your own copy of the disc; the loader checks the game files against known hashes (`config/xbe.sha1`). The supported version is the PAL release (title ID 49470018 v1).
- **Independent work:** this is not based on, and doesn't use code from, any other JSRF decompilation or port.
- **AI use:** most of the code and notes are written with [Claude Code](https://claude.com/claude-code), under human direction and testing.

## Build and run (developers)

Requirements: Windows 10/11, Visual Studio 2026 with **Desktop development with C++** (x86), Python 3, and your own disc dump.

1. Build: `tools\loader\build.bat` (writes `build\loader\ReJetLoader.exe` and `rejet.dll`).
2. Extract your disc: `python tools\xiso.py extract-all path\to\your.iso %USERPROFILE%\GameData\JSRF`.
3. Run: `play.bat`. The log is `build\loader\loader.log`. Settings (`[Console] Language`, `PAL60`) go in `rejet.ini` next to the loader.

## Repository layout

```
loader/       ReJetLoader.exe (address-space stub) and rejet.dll (XBE mapper, kernel, HLE patches)
tools/        xiso.py (disc reader), xbeinfo.py, doctor.py, loader build and codegen, Ghidra headless scripts, inventory
config/       hashes of the supported disc and XBE, function inventory, kernel imports, .text regions
include/xbox/ clean-room Xbox declarations
docs/         progress log, subsystem map, XDK census, lessons from ReWhacked
```

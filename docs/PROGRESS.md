# Progress

## 2026-10-27 — Phase 0: project set up

- Repo created (ReJet, in `Development\JSRF`). Generic tools copied from ReWhacked and renamed: `xiso.py`, `xbeinfo.py`, `doctor.py`, the Ghidra headless scripts, the inventory scripts, `d3d_abi.py`, `vsh_decode.py`, `gen_kernel.py`, `include/xbox/`. The loader itself is copied in Phase 2, once JSRF addresses are known.
- Disc and XBE hashed (`config/`). `orig/Default.xbe` extracted. PAL, title 49470018 v1, XDK 4134, no XNET/XONLINE, CRI ADX audio in `.text`, no movie files. Details in CLAUDE.md.
- Lessons from ReWhacked: `docs/REWHACKED_LESSONS.md`.
- User decisions: copy and adapt ReWhacked's code (no shared library); stay independent of the existing NA-disc JSRF decompilation.

## 2026-10-27 — Phase 1 started: first headless Ghidra pass

- `python tools/ghidra/export.py --fresh` (kernel thunk address set to 0x1C4A20 for this XBE) runs clean. FixupFunctions: 5,967 → 8,388 functions (+1,684 from pointers, +735 orphan runs), as in ReWhacked.
- Exported: 8,280 functions (.text 7,259, DSOUND 408, D3D 294, XPP 222, MMATRIX 41), 396 vtables, 120 kernel thunks, 35,855 call rows (`build/`, not committed).
- XbSymbolDatabase named 445 functions (D3D 130, DSOUND 137, XPP 16, XGRPH 4, 158 in .text, CRT among them).
- Next: write `config/regions.csv` (`.text` boundaries: game, CRI ADX, XAPI, CRT) and run `tools/inventory/build_inventory.py`.

## 2026-10-27 — Phase 1: inventory v1 and XDK census

- `config/regions.csv` splits `.text` by origin (from XbSymbolDatabase names, CRI string references, call patterns and instruction mix):

  | Range | Origin | What |
  |---|---|---|
  | 0x11000–0x13B010 | game | game code; x87 floating point (22,874 x87 vs 5 SSE instructions) |
  | 0x13B010–0x145FF0 | adx | CRI ADX audio middleware |
  | 0x145FF0–0x14C25A | xdk | XAPILIB; kernel jmp stubs at the end |
  | 0x14C25A–0x17CA26 | engine | unnamed libraries linked after XAPI: hand-written **SSE** vector maths (1,835 SSE vs 865 x87), then blocks heavy in D3D, XPP and DSOUND calls; probably Smilebit's engine libraries |
  | 0x17CA26–0x187430 | crt | LIBCMT/LIBCPMT |
  | 0x187430–0x18D600 | game | EH unwind funclets and static initialisers/atexit thunks |

  The game/ADX and ADX/XAPI edges are approximate (to within a few functions).
- `python tools/inventory/build_inventory.py` → `config/functions.csv`, `config/xdk_census.csv`, `config/vtables.csv`, `docs/XDK_CENSUS.md`. Counts: game 5,132 · engine 1,260 · xdk 1,146 · adx 419 · crt 277 · in data sections 46. Vtables 396 (game 299, engine 79). Kernel: 104 of 120 thunk imports are called (1,187 sites). `config/kernel_imports.csv` is the ExportThunks output (same format as ReWhacked).
- **Key finding: game code never calls D3D or DSOUND itself** (2 DSOUND sites), and reaches XPP only for device lists and memory units. Graphics, sound and input go through the engine layer, which makes 356 calls into D3D (103 distinct functions, 100 named), 172 into DSOUND (53 distinct, 50 named) and 10 into XPP. ADX calls DSOUND 14 times. Game code calls MMATRIX (an XDK maths section, 40 unnamed functions) 4,123 times.
  - So the shim worklist is the engine layer's XDK calls, and the engine layer is the natural place for enhancement hooks (projection, viewport, present, input), much like ReWhacked's renderer vtable.
  - Unlike Whacked!, part of the code (the engine maths) is SSE: any later rewrite there must keep SSE semantics, not just `/arch:IA32`.
- Next (Phase 1 continued): name the engine layer's entry points (renderer, sound, input), and locate video mode / present parameters, projection and viewport, the controller poll, the game timer and the Z: cache install.

## 2026-10-27 — Phase 1: engine layer mapped (`docs/SUBSYSTEMS.md`)

- The engine layer is COM-style C++ (interface GUIDs, QueryInterface): a PC-style renderer (vtable 0x1E6BA0, 121 slots), an input manager and pads with DirectInput-style state, and a sound object over DSOUND.
- **Video:** the game asks for 640x480, **16-bit colour**, 2 back buffers, vsync (`0x5FA70`). Device creation `0x157510`; mode pick `0x157400`.
- **Projection:** renderer slot 31 `0x154A40` takes a Sega-style angle (0x4000 = 90°) and near/far; the horizontal FOV is fixed, so wider viewports would be Vert-. Hor+ = one hook on slot 31. Matrix builder `0x154920`, cached at 0x26B7B8.
- **Timing:** no game delta timer found; QPC is used for profiling and load timeouts only, so logic is probably one step per frame at vsync. The CRI ADX server runs on its own thread once per vblank (`0x13BC70`), so the loader's vblank must be steady. PAL 50 Hz would be slower unless PAL60.
- **Input:** pad poll `0x167C50` (pad vtable 0x1E94C8, slot 25), with rumble via `XInputSetState`.
- **Sound:** DirectSound create `0x168F80`, per-frame work `0x169050`, 3D voice parameters `0x16DF00`; ADX fills its own buffers.
- Next: Z: cache install, the camera, language and 50/60 Hz selection; then Phase 2 (copy and adapt the loader).

## 2026-10-27 — Phase 1 complete: cache, language, 50/60 Hz, camera

- **Console settings**, named from their setting IDs: XGetLanguage `0x147D8B`, XGetVideoFlags `0x147DE1`, XGetAudioFlags `0x147E2D`, XGetVideoStandard `0x147DB8`, XGetAVPack `0x147DB0`.
- **Language:** the game language is at 0x258818 (0 Jpn, 1 Eng, 2 Frn, 3 Ger, 4 Spa, 5 Ita), set at start-up from XGetLanguage. A launcher option is the loader's answer to setting 7.
- **50/60 Hz:** device creation picks 50 Hz only for PAL without the PAL60 flag, so the loader can default to 60 Hz by reporting PAL60. The effect on game and ADX speed is to be checked at runtime.
- **Z: cache:** copy-on-read through `0x262E0`/`0x25910` (async, overlapped + unbuffered opens, write to `name~` then rename) and a background installer `0x25550` over 9 slots with `JSRF_CACHE_COMPLETE%02d.CMP` markers. The loader needs overlapped I/O and full sharing, and must not use host unbuffered I/O.
- **Camera:** MMATRIX is a Sega-style matrix stack, with LookAt at `0x1BBA30`. Six camera classes (vtables 0x1CB800–0x1CBA90) each set the viewport, call SetProjection with their own angle (+0x74; near 1, far 5000) and call LookAt. These are the hooks for per-camera widescreen and modern camera controls. Which class is the gameplay camera is found at runtime.
- Details are in `docs/SUBSYSTEMS.md`. **Phase 1 is done.** Next: Phase 2, copy and adapt ReWhacked's loader.

## 2026-10-27 — Phase 2: loader copied and adapted

- Copied from ReWhacked: the stub exe (`loader/stub`, `/BASE:0x10000` reservation), `loader.c` (XBE mapping, chimera PE header, crash logging), `emu_thread.c` (fs:/TLS), the kernel (`kernel/*.c`), the HLE framework, `perf.c`, and the generators `gen_kernel.py` and `gen_fs_patches.py`. Not copied yet: the D3D, DSOUND, XPP and network shims, the game hooks and the launcher. Their patch tables assume XDK 4831 calling conventions and data layouts and come over one by one in Phase 3.
- Names: `ReJetLoader.exe`, `rejet.dll`, `rejet.ini`, `REJET_*` debug switches, `%LOCALAPPDATA%\rejet\hdd`.
- **New `tools/loader/gen_xdk_addrs.py`** → `loader/core/xdk_addrs.gen.h`: a `#define` for each of the 401 uniquely named library functions in `config/functions.csv`, so patch tables refer to `XDK_XAPILIB_QueryPerformanceCounter` rather than a bare number.
- `hle.c`: JSRF's XBE SHA-1; the QueryPerformanceCounter patch (XDK 4134's XAPI is `rdtsc` with a constant 733,333,333 frequency, as in 4831) and SetThreadPriority.
- `kernel/hal.c`: a PAL console (AV region PAL-I, game region "rest of world", which the XBE allows), the language from `[Console] Language` (default English) and the PAL60 video flag from `[Console] PAL60` (default on, so the game runs at 60 Hz).
- `kernel/io.c`, for JSRF's Z: cache: host opens drop `FILE_NO_INTERMEDIATE_BUFFERING` (the game's buffers are aligned for the Xbox, not the host disk), and `NtSetInformationFile` supports FileRenameInformation (Xbox ANSI path → host UTF-16 NT path), for its `name~` → `name` renames.
- `loader.c`: the code range used for backtraces and fault filtering now comes from the XBE (headers to the kernel thunk at the start of `.rdata`) instead of Whacked!'s constants.
- `gen_kernel.py` only notes kernel functions that the loader implements but this XBE doesn't import (21 that Whacked! needed). JSRF: 120 imports, 100 implemented, 10 stubbed, 10 data. `gen_fs_patches.py`: 24 `fs:` sites (XAPI, DSOUND, CRT, ADX and engine regions).
- Builds clean with `tools\loader\build.bat` (VS 2026, x86).
- `kernel/rtl.c`: JSRF's XAPI has **statically pre-initialised critical sections** (for example the thread-notify lock at 0x234848 starts with the kernel event header `0x00040001`, type 1, size 4), where Whacked!'s were all zero. The loader keeps its host lock pointer in that first dword, so it took the header for a pointer and Windows raised `C0000008` (invalid handle) on the first RtlEnterCriticalSection. Now zero or any unaligned value (malloc'd pointers are 8-byte aligned) means "not set up yet".

### Phase 2 result (2026-10-27)

`play.bat` (or `build\loader\ReJetLoader.exe %USERPROFILE%\GameData\JSRF\default.xbe [seconds]`, disc extracted with `tools\xiso.py extract-all`):
- all 11 sections mapped at their original addresses, 120/120 kernel thunks patched, TLS set up, 24 `fs:` sites redirected, 2 HLE patches;
- XAPI start-up completes: `T:`, `U:` and `Z:` mounted (`%LOCALAPPDATA%\rejet\hdd`), TitleMeta/TitleImage/SaveImage written, heaps allocated;
- the game's static initialiser reads the console settings (language 1 = English, AV region PAL-I, PAL60 video flag), and `AvSendTVEncoderOption` is called twice;
- the game reaches its **renderer setup** (`0x5FA70`; the 640x480, 16 bpp display descriptor is on the stack). Device creation fails because the original XDK D3D has no GPU, and the game then reads a null renderer pointer at `0x5FC12`. This is where Phase 3 starts: the D3D8 → Direct3D 9Ex shim.

### Next (Phase 3)
1. Port the D3D shim from ReWhacked for XDK 4134. Run `tools/loader/d3d_abi.py` on this XBE to derive each function's calling convention, find D3D's render-state and texture-state arrays and dirty flags, check the resource header layout, and use `XDK_D3D8_*` names in the patch table. First goal: the device creates, and Clear + Swap show a window.
2. Then the DSOUND mixer (ADX sits on top of it) and XPP → XInput, each re-checked the same way.
3. Run with `REJET_TRACE=1` after each step; the first unimplemented kernel call stops with a `KERNEL <ordinal>` line.

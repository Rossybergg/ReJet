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

## 2026-10-27 — Phase 3 started: D3D shim milestone 1, XPP, and the DirectSound blocker

- **Why Phase 2 stopped (root cause):** XDK 4134's `D3D_GetAdapterModeCount` (0x19A2F0) and `EnumAdapterModes` ask the kernel for the AV capabilities (`AvSendTVEncoderOption` option 6) and keep only display modes whose refresh bits (mask 0xC00000) match. Our kernel answered 0, so there were no modes and the renderer never created a device. `kernel/hal.c` now answers `AV_PACK_SCART | AV_STANDARD_PAL_I | AV_FLAGS_60Hz` (50 Hz without PAL60).
- **D3D calling conventions:** `tools/loader/d3d_abi.py` (pointed at JSRF's D3D section) → `config/d3d_abi.csv`: the 103 D3D functions the engine calls, with argument bytes and register use. Almost all are stdcall. `SetRenderState_Simple` takes ecx/edx (NV2A method, value), and in 4134 it writes the push buffer (0x19BCC0) directly. Six end in a jump (`Clear` 24 bytes, `CreateImageSurface` 16, `LockBox` 20, `FrontFace` → `CullMode`, `Get2DSurfaceDesc`, `MmFreeContiguousMemory`). XDK 4134 uses out-parameters where 4831 returned pointers (`CreateTexture(..., ppTexture)` instead of `CreateTexture2`).
- **D3D data:** `D3D__pDevice` is 0x19E7A0. No `.text` code touches it or the push buffer, so patching every called D3D function is enough (no inline state writes to catch, unlike Whacked!'s state arrays).
- **`loader/core/d3d/d3d.c` (new, built from ReWhacked's):** window thread, Direct3D 9Ex device at the settings resolution (default 1280x960; 32-bit back buffer for the game's 16-bit one), stand-in Xbox device at 0x19E7A0, Clear, Swap, a vblank on a fixed 60/50 Hz grid that both the game thread and the ADX thread can wait on (callback under a lock), resources as Xbox-layout objects in game memory (textures, volume textures, image surfaces, vertex/index buffers, surface levels, locks), `D3DSurface_GetDesc`/`Get2DSurfaceDesc` (the original reads real-device fields), shader handles. **92 D3D patches**; render state, transforms, lights, viewport, textures, shaders and draws are typed stubs (exact argument bytes) that log their first call.
- **`loader/core/input/xpp.c`:** ReWhacked's XInput shim (4 ports, keyboard on port 1, `REJET_INPUT` scripted presses, `REJET_AUTOPRESS`), minus Whacked!'s modern controls. XDK 4134's XPP signatures match 4831's. Gamepad device type is 0x1BD320; the engine's `XInitDevices` is the jmp stub at 0x1BE0BA.
- `[Display] Width`, `Height`, `WindowScale`, `VSync` in `rejet.ini`.
- **Run result:** the window opens and the device is created (the game asks for 640x480, back buffer 0x11 (LIN_R5G6B5) x2, depth 0x2E (LIN_D24S8), 60 Hz, interval ONE). The renderer's Initialize finishes (render states, transforms, viewport, lights, gamma), input starts, and the engine creates its first textures (128x128, 512x512 R5G6B5) and vertex shaders. These are NV2A programs with the same `0x....2078` header as Whacked!'s, so ReWhacked's translator should apply.
- **Blocker: DirectSound.** The original `DirectSoundCreate` programs the APU (reads 0xFE801100). With an interim "no driver" patch (`hle.c`), the engine's sound object fails, the renderer setup (`0x5FA70`) returns that error, and the game tears its renderer down and returns to the dashboard (`XLaunchNewImage` → `HalReturnToFirmware(2)`). **JSRF won't start without DirectSound**, so no frame has been presented yet.
- Fix on the way: the engine releases with `while (Release() != 0)` loops, so `D3DDevice_Release` reports the last reference (it hung the thread before).

### Next
1. Port ReWhacked's DSOUND software mixer (`audio/dsound.c`, XAudio2) for XDK 4134: argument bytes for the 53 DSOUND functions the engine and ADX call (the same approach as `d3d_abi.py`), then replace the interim "no driver" patch. Goal: the game passes sound start-up and calls Swap (the window shows cleared frames).
2. Then bring over the D3D pieces in order: render state (`SetRenderState_Simple` NV2A method mapping), viewport and transforms, textures (`tex.c`), vertex programs (`vsh.c`), register combiners (`psh.c`), draws, including 4134's immediate-mode `Begin`/`SetVertexData*`/`End` and `SetTile`.

## 2026-10-27 — Phase 3: DirectSound mixer; the game runs to the title screen with music

- **`tools/loader/xdk_abi.py`** replaces `d3d_abi.py`: any XBE section (`python tools/loader/xdk_abi.py DSOUND`), read from the XBE header, following `jmp` stubs to their body (that also filled the 3 blank rows of `config/d3d_abi.csv`). `config/dsound_abi.csv`: the 53 DSOUND functions the engine and ADX call, all stdcall.
- **XDK 4134 DirectSound differences:** speaker routing is a mix-bin **mask** (DSBUFFERDESC field 4, `SetMixBins(DWORD)`), not 4831's DSMIXBINS pointer; buffers have `SetLoopRegion`; no streams or file media objects are used. ADX's buffer description (0x141E50): size 0x18, flags 0x400A0, 16-bit PCM 44.1 kHz stereo, mask 0. 0x19EF7C/0x19EF81 only return 0 (left as original); 0x19EFE3 takes DirectSound's internal lock (patched to a no-op).
- **`loader/core/audio/dsound.c`** (from ReWhacked's mixer): formats, Xbox ADPCM decoder, voices, buffers (create, lock/unlock, play/stop, position, frequency, volume, headroom, loop region, mask), the 48 kHz mixer with ReWhacked's limiter, XAudio2 output, `REJET_AUDIODUMP`/`REJET_NOAUDIO`/`REJET_VOLUME`. 3D positioning, listener, effects (`GetEffectData` returns zeros, `SetEffectData`), envelopes, filters and LFOs are typed no-ops. `[Audio] Volume`, `MuteInBackground` in `rejet.ini`.
- **Kernel/XAPI:** `NtUserIoApcDispatcher` (ordinal 232) implemented: XAPI passes it as the APC routine of overlapped reads (`ReadFileEx`), so Windows calls it; it calls the caller's completion routine with the error, byte count and OVERLAPPED. `GetThreadPriority` and `SetThreadPriorityBoost` go straight to the host (like ReWhacked's `SetThreadPriority`; the XAPI versions need `ObReferenceObjectByHandle`).
- **CRI ADX assumes one CPU (found with a new thread dump, below).** Its lock (0x13BB50) raises the caller to priority 15 and resumes a priority-2 spinner thread (0x13BC30) so that nothing at priority 1 or below runs; unlock (0x13BB90) suspends it again. Two consequences on a PC:
  1. **Every Xbox thread now runs on one host core** (`emu_thread.c`: the highest core of the process; `REJET_XBOXCPU=<n>` or `=all`), so Windows' priority scheduling stands in for the Xbox's. Host threads (window, D3D, mixer, XAudio2) use the other cores.
  2. **Threads are never created suspended on the host** (`kernel/ps.c`). ADX's spinner was created suspended, resumed, and suspended again while Windows was still starting it, so it sat suspended *holding the loader lock*: XAudio2's mastering voice took ~10 s to appear and `ExitProcess` hung until ADX resumed the spinner. Now the thread starts, reports from our code, and is suspended there (suspend count 1) if the Xbox asked for a suspended thread.
- The vblank wait (`d3d.c`) now sleeps on a per-thread high-resolution waitable timer instead of spinning with `Sleep(0)` (2.6 instead of 58 CPU-seconds in 20 s).
- **Diagnostics:** `loader.c` writes a thread dump (loader-lock owner, each thread's priority, instruction pointer as module+offset, XBE backtrace) on timeout and after `REJET_HANGDUMP=<seconds>`.
- **Run result (no picture yet: draws are still stubs):**
  - start-up completes; the game presents at a steady 60 Hz (frame N at vblank N) and exits cleanly;
  - it loads fonts, the sound DSP image, system sounds, the save area on `U:`, the title logo and stage 10; the Z: copy-on-read cache works (writes `name~`, renames);
  - with scripted presses (`REJET_INPUT='12:start,15:start,17:a,19:a,21:start,24:a'`) it loads Beat, Gum and Yoyo and opens `Z_ADX\BGM\title.adx`; ADX plays a looping 16-bit 44.1 kHz stereo buffer, and **the title music is audible**: the recorded mix (`REJET_AUDIODUMP=1`) is silent until 32 s, then RMS 1600 → 4200 as the track fades in, peak 0.6.

### Next
1. Render state: `SetRenderState_Simple` (NV2A methods, ReWhacked's mapping) and the `SetRenderState_*`/`SetTextureState_*` functions; viewport and transforms.
2. Textures (`tex.c`: swizzled, DXT and linear formats; the game uses DXT1/DXT2-3 (0x0C/0x0E), X8R8G8B8 and R5G6B5) and `SetTexture`.
3. Vertex programs (`vsh.c`; the game's programs have the same NV2A `0x....2078` header as Whacked!'s), register combiners (`psh.c`), streams, indices, and the draws, including 4134's immediate mode (`Begin`/`SetVertexData*`/`End`) and `SetTile`/`GetTile`.
4. Engine sound effects: the engine creates its ADPCM buffers with 0 bytes and fills them later (no plays seen yet); check once menus can be navigated with a picture.

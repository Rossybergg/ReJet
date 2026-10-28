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

## 2026-10-27 — Phase 3: rendering; the title screen and attract mode draw in 3D

- **D3D state in XDK 4134 (same model as Whacked!):** the engine writes D3D's state arrays inline and calls D3D only for the rest. Texture state `TextureState[4][32]` at **0x19E9A0** (4831 indices; TexCoordIndex at 0x19EA10 and ColorKeyColor at 0x19EA18 confirm it), dirty flags 0x19E998, render-state array 0x19EBA0, `D3D__IndexData` **0x19E994** (the engine computes DrawIndexedVertices' index pointer from it). `SetRenderState_Simple` passes the whole NV2A method word (count bits included, 0x0004035C), so only the low bits are mapped.
- `SetVertexShaderConstant(Register, pData, Count)` takes D3D register numbers and registers (it adds 0x60 itself: c0 is NV2A 96); 4831 had NV2A numbers and dword counts.
- **Ported from ReWhacked:** `vsh.c` (NV2A vertex programs → HLSL), `psh.c` (register combiners → HLSL), `tex.c` (swizzled, DXT, linear, volume, cube textures), `shadercache.c` (`%LOCALAPPDATA%\rejet\shadercache`), and in `d3d.c`: vertex/pixel shader creation and binding, constants, `prepare_draw`, NV2A render-state mapping, the 1-argument render states, texture-stage state from the array, viewport with scissor (split screen), transforms (kept for GetTransform, passed to the host for fixed-function draws), lights and materials, SetTexture, streams, indices, and the draws (`DrawVertices`, `DrawIndexedVertices`, `DrawVerticesUP`, `DrawIndexedVerticesUP`, quads → triangles, XYZRHW scaled to the render resolution). Screenshots: F12 → `Screenshots\`, `REJET_SHOTS=<frames>`; `REJET_DUMPVS=1` dumps programs and declarations. HD texture packs are stubbed (`hdtex.h`).
- **Vertex declarations:** JSRF uses `D3DVSDT_PBYTE4` (0x44) for vertex colours, which ReWhacked's parser didn't know (every program's declaration failed). `vsh_declaration` now maps the Xbox types with a D3D9 equivalent: FLOAT1-4, D3DCOLOR, PBYTE4 (UBYTE4N), SHORT2/4, NORMSHORT2/4; others are logged.
- **Result (1280x960):**
  - the SEGA logo and the anti-graffiti notice draw correctly (30 fps on those screens);
  - the **title screen draws in full 3D** (the city with characters, the JSRF logo and copyright) and the **attract mode** shows a cel-shaded skater with outlines, motion lines, "PLEASE PRESS START TO BEGIN" and the logo, at a full **60 fps** (600 frames in 605 vblanks, about 270,000 draws per 10 s);
  - 254 vertex programs created; no render-state method unmapped; no draws skipped; about 10% of draws untextured (to check).
- Still stubs: immediate mode (`Begin`/`SetVertexData2f`/`SetVertexData4f`/`End`, used by the engine at 0x151E4E, probably fades or overlays), render-to-texture (draws into other targets are skipped; none seen yet on these screens), `SetTile`/`GetTile`, gamma, the multisample/z-bias/line-width render states, `CopyRects`.

### Next
1. Immediate mode (Begin/SetVertexData/End) so fades and overlays draw; then render-to-texture (the engine's 512x512 and 128x128 R5G6B5 targets).
2. Play into a stage (scripted input) and fix what shows up; compare with xemu or real hardware screenshots where unsure.
3. Then Phase 4 enhancements (launcher, widescreen via renderer slot 31, modern camera).

## 2026-10-27 — Phase 3: immediate mode, render-to-texture; **playable in the Garage**

- **Immediate mode** (`Begin`/`SetVertexData2f`/`SetVertexData4f`/`End`): the engine draws full-screen fades (0x151DA0: a strip with diffuse colour and stencil) and debug text (0x1522B0) this way, with FVF 0x1C4 (XYZRHW, diffuse, specular, 1 texcoord). Registers are the Xbox D3DVSDE numbers (0 position, 2 normal, 3 diffuse, 4 specular, 9-12 texcoords); -1 sets the position and emits the vertex. `d3d.c` keeps the current register values, collects vertices, packs them in the current FVF's layout at `End` and draws them like `DrawVerticesUP`. With the fades drawn, the logo screens run at 60 fps instead of 30.
- **Render-to-texture:** each Xbox texture used as a target gets a host render-target texture at the render scale with its own D24S8 depth surface (found by data address; bound by `prepare_draw` wherever the game samples that texture, with texel scaling for linear formats). Clear, viewport/scissor, pre-transformed vertices and the 2D screen constant now use the current target's size and scale. **Not exercised yet:** no render target was set anywhere in the logos, title, attract mode, opening cutscene or Garage.
- **`tools/contact_sheet.py`**: a labelled contact sheet of `shot_NNNNN.bmp` files (standard library only), for checking scripted runs at a glance.
- **Playing (scripted input, 1280x960, 60 fps throughout, no errors):**
  - Start on the title screen starts a new game: the opening event in the **GG's Garage** (letterboxed 3D cutscene with speech bubbles: "This is the GG's Garage. Hey, where's our pizza?");
  - A advances the dialogue, then **gameplay**: Yoyo skates around the Garage with the HUD (health, spray cans, boost gauge), jumps, lands a "Method" trick that scores, and **grinds rails** with speed lines; the camera follows correctly;
  - script used: `REJET_INPUT` with Start at 36-42 s, A every 1.5 s from 45 s, then chained `ls_up`/`ls_left`/`ls_right` presses (0.2 s each) to hold the stick.
- **Known visual gaps:** no ground shadow under the characters, and the cel outlines look thin or missing; about 30 untextured draws per frame in the Garage (to check what they are); gamma ramps, tiling (`SetTile`/`GetTile`), z-bias, line width and multisample render states are still stubs; immediate mode with a vertex program isn't handled (not used so far).

### Next
1. Character shadows and outlines: find how the engine draws them (stencil, render target, or a texture pass) and what is missing.
2. Explore further by script (leave the Garage into Shibuya-cho) and fix what shows up.
3. Phase 4: a launcher and the enhancements (widescreen via renderer slot 31 and the camera angle at +0x74, modern camera, MSAA/anisotropy from ReWhacked).

## 2026-10-27 — Shadows and outlines: investigation (in progress, uncommitted)

- Added `REJET_DRAWLOG=<frame>` (per-draw shader, texture, blend, alpha test, depth, stencil, cull), a log of the first frame in which each vertex program counts stencil, and FrontFace recording (FrontFace is never called in play).
- **JSRF shadows are stencil shadow volumes:** volumes drawn twice (back faces INCRSAT, front faces DECRSAT, depth GREATEREQUAL, no colour), then a full-screen black quad at alpha 0.5 (immediate mode, FVF 0x1C4, stencil LESSEQUAL 1). Environment volumes use fixed-function FVF 0x42; vertex program 31 draws volumes too (seen only at frame ~2316, not in the Garage).
- Fixes so far: pre-transformed (XYZRHW) z is divided by the 24-bit depth scale (the quad was behind the scene); fixed-function stages with no texture read white (D3D9 gave alpha 0 and the quad vanished). With these, the quad covers the screen and the stencil marks the environment's shadow regions; volume geometry is in the right place (checked in wireframe).
- Host vsync is now off by default: with it on, Present waited two monitor refreshes and halved the frame rate in unattended runs.
- **Still open:** no visible shadow under the player in the Garage (no character volume drawn there: find how the character shadow is drawn); outlines (vertex program 29 inverted hull, untextured) not yet checked after the white-texture fix. `REJET_SHADOWDEBUG` (1/2/3) in d3d.c is a temporary debugging block to remove when done.

### 2026-10-27 — after the user's first play test (10 minutes, performs well)

- **Reported:** speech bubbles hidden behind the cutscene bars; boosting turns the screen grey and red; white instead of black cel outlines on some faces.
- **Empty texture stages, corrected:** a fixed-function stage with no texture whose operation reads the texture is now switched off (the colour passes through), instead of binding a white texture. White made the untextured outline hulls white; opaque black blacked out the cutscene sky strip (a 257-vertex untextured strip with vertex colours); D3D9's default made the shadow quad and sky vanish. "Switched off" fits all of them.
- **Pre-transformed z is 0..1 after all:** the earlier "depth-buffer units" scaling squeezed every 2D element to ~0 and hid the speech bubbles behind the letterbox bars; removed. (The shadow quad at z = 0.5 passes LEQUAL in front of nearly everything; it was never hidden by depth.)
- **CopyRects implemented** (GPU StretchRect from the back buffer or a render target into a render-target texture): the play test created the 512x512 and 128x128 targets, used by the boost effect; the copy was a stub. Not yet verified (the boost can't be reached by script yet).
- Verified by script: cutscene bubbles readable over the bars, Garage gameplay at 60 fps. Waiting on the user's retest of the boost and the outlines.

### 2026-10-27 — second play test: open issues for next session

- **Fixed and confirmed by the user:** speech bubbles over the cutscene bars.
- **Still broken: boost** (10 cans + B): the screen goes grey and red until the boost ends. The play log shows `CopyRects` is **never called**; the game renders into its two render textures itself (512x512 and 128x128, format 0x11 LIN_R5G6B5, at data 0x0D830000 / 0x0D9B0000; host 1024x1024 / 256x256) and draws with them. Suspects, in order: texcoord scaling when sampling a linear render target (we set 1/size; check the vertex program's texcoords and VSH_C_TEXSCALE), the per-target viewport/scissor/screen constants during the off-screen pass, `SetTile`/`GetTile` (still stubs; Xbox tiling of render-target memory), and R5G6B5 having no alpha. Next: `REJET_DRAWLOG` on a boost frame (needs a save near cans) and `REJET_SHOTS`.
- **New: map screen is solid white** (Start in the main area). Not investigated yet; check its draws with `REJET_DRAWLOG` (possibly a render target, a texture format, or the empty-stage rule).
- **Outlines:** the user didn't report white face outlines this time (the empty-stage rule fixed them, to confirm).
- **Saves:** the user will play manually to make a save past the training, so tests can start from it. Saves live in `%LOCALAPPDATA%\rejet\hdd\Partition1\UDATA\49470018\` (only TitleMeta/TitleImage/SaveImage there so far). Keep that folder; don't point test runs at a fresh `LOCALAPPDATA`.
- Play-session logs kept: `build\loader\loader_user_session.log` and `loader_user_session2.log` (git-ignored).

## 2026-10-28 — Testing tools; boost and map screen

- **Testing tools for manual play** (`docs/TESTING.md`): F12 screenshot, F11 bug capture (screen, every draw with its state, render targets as images, clears, the vertices of small draws, materials and lights, the Xbox render-state array), F10 log marker; the previous session's log is kept in `logs\`. F10 arrives as WM_SYSKEYDOWN (handled now).
- **Boost:** the user's capture showed the 512x512 render target held the right picture; the full-screen composite (fixed-function FVF 0x1C4, BLENDTEXTUREALPHA) used texel coordinates for the linear texture. Fixed-function draws now scale texcoords of linear / render-target stages on the CPU (`fix_ff_vertices`). User: the boost "looks much better" but darkened. The composite blends the new frame over the previous one (speed trails on the Xbox, per the user's reference video); with D3DSWAPEFFECT_DISCARD the previous frame was lost. The swap effect is now COPY. **To confirm by the user.**
- **Map screen fixed** (reproduced by script: Start in the Garage, `REJET_CAPTURE_AT`): it is drawn with fixed-function lighting and materials, which D3D applies lazily from its render-state array at 0x19EBA0 (XDK order: LIGHTING 92, SPECULARENABLE 93, LOCALVIEWER 94, COLORVERTEX 95, material sources 100-103, AMBIENT 105; fog start/end at 84/85 and max point size 64.0 at 113 confirm it). These are applied per fixed-function draw now. Lit draws without normals (the counter panel and icons) get D3D9's ambient-only black; the NV2A keeps the last normal, so they are drawn unlit with the material's fully lit colour as the texture factor. Matches the user's reference photo (the counter dot is orange on the Xbox too, checked by the user).
- **Seen, not a regression:** a large white shape covers the lower part of the screen in the intro fly-through (frame ~2400); same with the previous commit. To check against the original.

## 2026-10-28 — The ReJet menu (Phase 4 starts)

- **In-game ReJet menu** (`loader/core/ui/menu.c`), the user's choice over a ReWhacked-style launcher, built in-house (no outside libraries): F1 or Back + Start on any controller pauses the game (the game thread stays in Swap and the menu redraws over a frozen copy of the frame; music continues at 35%). JSRF-style look: GDI font atlas (Arial Black italic), lime/pink parallelograms, one texture, device state saved and restored with a state block. Pages: Resume, Report a bug, Display, Audio, Game, Controls, Quit. Controllers are read directly with XInput (d-pad/stick with repeat, A, B, Start); the keyboard through the window thread's messages (arrows, Enter, Esc, F1; typed text for the bug note). While the menu is open, and until its buttons are released, the game sees idle controllers; while Back is held, Start goes to the menu only.
- **Settings are saved** to `rejet.ini` on every change (`settings_save`). New: Fullscreen (borderless), Anisotropy, ShowFPS. Live: window size, full screen, texture filtering, FPS counter, volume, mute in background. On the next start: resolution, VSync, language. The PAL60 setting is gone: the console always reports 60 Hz.
- **Report a bug** writes the category and note into the next bug capture (`note.txt`). On-screen messages confirm F10 markers, F11 captures and F12 screenshots.
- ReJet is now **DPI aware** (sharp on scaled displays; the window size accounts for display scaling), and the bars around the picture are repainted black after a resize (full screen showed stale frames there).
- Verified by script (keys posted to the window, screen grabs): menu pages, settings saved, bug report capture with its note, live full screen and window size. Not verified: controller navigation (no controller on the test machine): the user's play test.
- **Boost, the user's capture with vertex data:** the composite is drawn at alpha 0x8C (55%) over a back buffer cleared to black, which is the darkening; the scene was drawn into the 128x128 target this frame while the composite samples the 512x512 one. Captures now log render-target switches and CopyRects, to see how the 512x512 target is filled. Swap effect COPY didn't help (to revert with MSAA).
- `docs/ENHANCEMENTS.md`: what's done and the candidate list.

### 2026-10-28 — Menu animations, logo, live resolution

- **Live resolution and VSync** (`change_display` in d3d.c): `ResetEx` with the new back-buffer size and presentation interval; D3D9Ex keeps textures, shaders and buffers, so only the device state ReJet set (render states through `rs()`, transforms, material, lights, the current target) is put back, and the render-target textures are recreated at the new scale. Verified by script: 1280x960 → 1440x1080 → 960x720 → 1280x960 from the menu, the game drawing normally afterwards.
- **Window size is now its own setting** (`WindowHeight` in rejet.ini, 4:3, fitted to the screen's work area), independent of the resolution: before, it was a percentage of the resolution, so the user saw the window grow while the picture stayed at the same internal resolution.
- **Menu animations:** slides in from the sides and out again on close (the frozen frame darkens and brightens with it), page changes stagger the rows in from the direction of travel, the selection bar glides between rows, changed values nudge, the page name drops in, diagonal stripes drift behind, messages slide in.
- **ReJet logo** (the user's art, `loader/res/rejet_logo.png`, built into rejet.dll as resource `REJET_LOGO`; a `rejet_logo.png` next to the exe overrides it): decoded with WIC, transparent edges trimmed, box-filtered into the font atlas at the drawn size, shown in the title strip, which now sizes itself to the longest page name.

### 2026-10-28 — Widescreen, cleaner 2D at high resolution

- **Widescreen (Hor+)**, a new Aspect setting (4:3 / 16:10 / 16:9 / 21:9), live. The game's projection fixes the horizontal field of view (m00 = 1, m11 = 1.333 for 4:3); ReJet divides m00 by k = render aspect / (4:3) in place as the matrix passes through SetTransform (the engine keeps it in its own memory and builds its vertex-program constants from it), recognising an already-patched matrix, and re-patches the remembered matrix on a display change. The 640x480 screen space is stretched over the wide back buffer, so 3D keeps its proportions with more visible at the sides; 2D (HUD, bubbles) stretches. Cutscene bars span the width. Resolution is now chosen by height, the width following the aspect; the window takes the aspect's shape. Verified at 1920x1080 in the Garage and a cutscene, and switching 16:9 -> 16:10 live.
- **Dark seams around 2D art** (the user's report): DXT1's transparent texels decode to black, so bilinear filtering above 640x480 blended black into the edges of alpha-blended art. DXT textures with transparency are now decoded to A8R8G8B8 at upload and their transparent texels take the average colour of their opaque neighbours (a few texels deep); alpha is unchanged. Opaque DXT1 stays compressed.
- **Coloured lines along text boxes** ("Please press Start", cyan): the art comes from atlases (that texture holds the press-start text above the cyan title-menu panel); above 640x480 a quad's outermost pixels sample half a texel past its edge. Draws by vertex programs whose texcoord 0 is a straight copy of an input (vsh.c records it, e.g. program 116: `mov oT0.xy, v2`) now get each quad's texture coordinates pulled in by half a texel (single quads and non-indexed quad batches; a copy is drawn). Verified: the two 1-pixel lines at the quad's top and bottom are gone. Captures log `inset:` lines.
- **Menu text artifacts:** glyphs leaned into their neighbours in the font atlas (italic overhang, filtering); cells now have margins and gaps. The menu's title strip, footer and stripes run to the screen edges on wide screens.

### 2026-10-28 — Alpha preparation: widescreen 2D and culling, first-run setup, project page

- **Widescreen 2D** (the user: dialogue text running out of its box, the game's menus misaligned): UI drawn in screen pixels stretched with the screen while UI drawn through the projection got Hor+. Now 2D keeps 4:3 proportions in the middle, where projected UI lands (`squeeze_2d`): pre-transformed vertices squeezed on the CPU, 2D vertex programs through c[197], fixed-function geometry under an orthographic projection through a narrowed host projection; draws spanning the whole width (fades, cutscene bars, boost and shadow overlays) stay stretched. Verified at 16:9: speech bubbles and text agree, the map screen's labels and counters line up, 4:3 unchanged.
- **Edge pop-in:** the renderer's SetProjection (vtable 0x1E6BA0 slot 31, 0x154A40, stdcall this/fov/near/far) derives the horizontal culling planes 0x26B740..4C (1/cos, -tan, 1/cos, tan of fov/2) from the 4:3 angle; it is hooked through its vtable entry (its only reference) and the planes are widened by k after it runs. Widening the angle itself instead was tried and rejected: the engine's screen-space vertical (0x26B7B0 = -aspect x focal) and the matrix (1 / (aspect x tan)) use the aspect factor in opposite ways, which moved speech bubbles behind the cutscene bars. The map screen shows labels it culled before. Pop-in during play still to be confirmed by the user.
- **First-run setup** (`loader/core/ui/setup.c`; the user's request for ReWhacked's launcher workflow): run without arguments, ReJet uses `[Game] Folder` from rejet.ini, else `Game\` next to the exe, else shows a setup window (GDI, the menu's style and logo; mouse, keyboard, controller): choose the disc image (XDVDFS reader in C: trimmed or full images, XGD1-3 partition offsets) or an extracted folder; Default.xbe is checked against the supported SHA-1; images are extracted to `Game\` with progress, free-space check and cancel. Verified from an empty folder with the user's image: 3505 files, 2.4 GB, the same file list as `tools/xiso.py`, identical default.xbe, the game then started. The ReJet menu's Game page shows the folder and can ask again at the next start. `build.bat` finds Visual Studio with vswhere.
- **README rewritten as the project page** (description, screenshots in `docs/images/`, features, getting started, controls, known issues, bug reports, building, how it works, credits). Not pushed anywhere.

### 2026-10-28 — Versions and release packages

- **Semantic versioning** (the user): `VERSION` (0.1.0, the first alpha); `tools/loader/gen_version.py` (run by build.bat) writes `version.gen.h` (git-ignored) with the version and the build string `<version>+<commit>[-dirty]`. Shown in the ReJet menu (bottom right), the startup message, the setup screen, the window title (version); written in the first line of `loader.log` (with the Windows build), in captures' `info.txt` and bug-report `note.txt`; and in the Windows version resources of both files (`loader/res/version.rc`).
- **Release packages:** `python tools/make_release.py` builds and writes `dist/ReJet-<version>/` and the zip (ReJetLoader.exe, rejet.dll, a player readme from `docs/release/README.txt`; anything else is refused), from a clean tree unless `--allow-dirty`. Tested from an empty folder: first-run setup from the user's image, then the game, with the version in the log and menu.

### 2026-10-28 — 0.2.0: alpha polish (the user's UX review)

- **Presentation:** window title "ReJet - Jet Set Radio Future" (no key list); a ReJet icon made from the logo's R on the menu's lime badge (`tools/art/make_icon.ps1` -> `loader/res/rejet.ico`, 16-256 px) for the exe, the game and the setup windows; the exe is now **ReJet.exe** (the user renamed it while testing); a start-up splash drawn by the menu renderer (logo on the lime band, moving stripe, version) from the first frame until the game has drawn on 10 frames in a row (min 1.5 s), fading into the SEGA logo; the welcome message waits for it. (A first measurement claimed 6.5 s of black at start-up; that sampled too sparsely and read the dark SEGA screen as blank. The real black gap is about a second.)
- **Setup screen text** rewritten in a friendlier voice, errors included (PAL release named as "the European (PAL) Xbox release").
- **Display page:** "Internal resolution"; plainer help lines; VSync explained by tearing vs responsiveness; Window size greyed and skipped while full screen is on.
- **Texture filtering verified:** 16x shows the Garage floor's tile grid sharply into the distance where "Original" blurs it; the effect is mostly on surfaces at a shallow angle. It is not anti-aliasing (MSAA is planned).
- **Controller button icons:** drawn into the menu's atlas at its scale (circle, pill and d-pad shapes with smooth edges, tinted: A/B/X/Y in Xbox colours, LB/RB/LT/RT pills, Menu and View symbols), used in the hint bar when a controller is connected (keyboard keys otherwise; `REJET_PAD_ICONS=1` forces them for tests) and on the Controls page next to the keyboard keys.
- **High frame rate:** investigated and written up in `docs/HIGH_FRAME_RATE.md` (logic is frame-locked; a smooth-motion mode would mean interpolating draws between 60 Hz updates; recommended later as an experimental spike).
- `make_release.py` assembles packages in `dist/.stage` and only refreshes the program files in `dist/ReJet-<version>/`, which may be a test install with game files and settings (the old version deleted that folder first).

### 2026-10-28 — 0.2.1: licence, public-repo check, README

- **Licence: GPL-3.0** (the user's choice): the official text from gnu.org as `LICENSE` (SHA-256 3972dc97...b36986, the standard file); release zips carry it as `LICENSE.txt`.
- **Public-repo scan:** no personal paths, names or e-mail addresses in the 103 tracked files or the history (commit metadata aside: the author name, for the user to decide on); no game data (generated files and config hold addresses, hashes and metadata only). The supplied logo copy in the root is git-ignored.
- **README rewritten** in a conversational voice: what ReJet is, screenshots, requirements, which game version (the European PAL Xbox release only, checked automatically), getting started, controls, the ReJet menu, what's new, what's planned, known issues, bug reports, building, licence, credits.
- **Issue templates** (`.github/ISSUE_TEMPLATE/`): a bug-report form that walks people through Report a bug in the ReJet menu, then attaching the zipped capture and loader.log, with the ReJet version, category, PC and an own-copy check; an idea form; blank issues off. (The user lifted the `.github/` edit rule in `.claude/settings.json` for this.)

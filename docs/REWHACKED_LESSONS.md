# Lessons from ReWhacked

ReWhacked (`..\rewhacked`) took Whacked! (XDK 4831) from a disc dump to a playable Windows game with a launcher, widescreen, modern controls, split screen and LAN play from 2026-10-18 to 2026-10-21. This page lists what carried the most weight, so ReJet can skip the detours. Details and addresses are in that repo's `docs/PROGRESS.md`, `docs/LOADER.md` and `docs/KNOWN_BUGS.md`.

## Approach
- **Hybrid host, not decompilation first.** Map the original XBE at its own addresses, implement the kernel, and HLE-patch the XDK libraries with PC shims. The game's own code runs unchanged. Enhancements hook specific game functions. That reached "playable" in days. A full decomp is optional and later.
- **No byte matching needed.** Without a legitimate period compiler, acceptance comes from running the game (and later, replays and differential tests).
- **Every enhancement is behind a switch** with the original behaviour as default, set in a launcher that writes an ini file.
- **Map first (Phase 1), then boot.** The headless Ghidra pipeline (fresh import → `FixupFunctions.java` → CSV exports → `build_inventory.py`) gave the function inventory, the XDK call census (the shim worklist) and the vtable map. Run `FixupFunctions`: auto-analysis missed about a third of game functions (vtable and pointer targets only).

## Loader mechanics that had to be found the hard way
- A plain 32-bit process **can't reserve 0x10000–0x400000**. Use a stub exe linked `/BASE:0x10000 /FIXED /DYNAMICBASE:NO /SAFESEH:NO /NODEFAULTLIB` with a big reserved section; the loader DLL then maps the XBE over it.
- **Chimera header:** after the XBE header is copied to 0x10000, `CreateThread` fails (error 193) because Windows reads the PE header at run time. Write `MZ` and a cut-down NT header at 0x40 into the RSA signature area (never read on the Xbox); every XBE field from 0x104 on stays untouched.
- **Smart App Control** blocked the stub exe; the user had to turn it off. Expect the same here.
- **`fs:`/TLS:** library code reads `fs:` as a KPCR; redirect those reads to TEB TLS slots. Xbox TLS grows *down* from `fs:[4]`.
- `NtAllocateVirtualMemory` must return exactly the requested page range, or the heap corrupts.
- **Open host files with full sharing.** The Xbox allows a later exclusive open of a file that is still open; Windows doesn't. This caused a first-run black screen during the Z: cache install. JSRF has a Z: cache install too (`JSRF_CACHE_COMPLETE*.CMP`).
- XAPI first-boot needed FATX-like cluster geometry, a cache-partition table and format/dismount support for the HDD partitions. Partitions live in `%LOCALAPPDATA%\<project>\hdd`.
- Only treat faults in XBE code as fatal; drivers raise and catch their own exceptions. Log raw stack and registers on crashes.

## Timing
- **The "game runs too fast" bug was the clock, not vsync.** XAPI `QueryPerformanceCounter` is plain `rdtsc`, while `QueryPerformanceFrequency` returns a constant 733,333,333 (the Xbox CPU clock). HLE-patch the counter to return host time on a 733.33 MHz clock. Also run the game's vblank callback once per emulated vblank and report real vblank counts. Check this first in JSRF.
- PAL titles may run 50 Hz: decide early whether to present at 50 or offer PAL60.

## Graphics (Xbox D3D8 → D3D9Ex)
- Game code often doesn't call D3D for state: XDK macros are inlined and write D3D's **render-state and texture-state arrays** directly. Read state from those arrays at draw time (their addresses differ per XDK version and per game).
- **Derive calling conventions from machine code** (`tools/loader/d3d_abi.py`): XDK D3D functions use register conventions.
- Vertex shaders are NV2A microcode programs: translate to HLSL (`vsh.c`), validate the decoder offline on every dumped program (`vsh_decode.py`). The game reuses the buffer passed to `CreateVertexShader`: copy it. Internal constant setters take NV2A register numbers (c0 = 96).
- Pixel shaders are register-combiner programs: translate to HLSL (`psh.c`), variant per bound texture type.
- **Compile every shader at creation, with an on-disk cache.** Lazy compiles caused gameplay hitches; a first-run "preparing graphics" screen covers the one-time compile.
- Texture bugs that looked like lighting bugs: locking every mip with `D3DLOCK_DISCARD` dropped level 0 (close surfaces black). Linear textures need 64-byte pitch. Volume textures (3D swizzle, DXT in groups of 4 slices) carried particle effects.
- `SetViewport` must also set a scissor rect and `Clear` must clip to the viewport, or split screen spills.
- Index data: `SetIndices` sets a D3D global (`D3D__IndexData`); missing it dropped all indexed models.
- Any resolution: vertex programs output Xbox screen pixels, so scaling the epilogue scales 3D; pre-transformed (XYZRHW) 2D needs its own scaling.
- **Widescreen Hor+** was one hook on the game's projection builder (separate width/height extents), plus a 2D squeeze option. Find JSRF's projection builder in Phase 1.
- Shadow maps (depth textures used with shadow compare) were the hardest feature and are still parked. Vertex programs that never write oD0 need an alpha-1 default.

## Audio
- DSOUND → a software mixer over XAudio2 (part of Windows, no download): every buffer and stream is a voice, 48 kHz stereo, 10 ms chunks; Xbox ADPCM decoder that matches vgmstream sample for sample.
- Fold surround mix bins to stereo **without summing**, then a limiter (not a per-sample soft clip).
- **Releasing a stream must drop its queued packets**: the game reuses their memory at once (loud corrupt bursts otherwise).
- Diagnostics paid off: per-sound peak/saturation on first play, raw and decoded dumps (`SNDDUMP`), a final-mix WAV dump.
- In JSRF the audio middleware is CRI ADX running on top of DSOUND, so the same shim applies; vgmstream can check ADX decoding offline.

## Input and controls
- XPP → XInput for 4 ports, with a keyboard fallback on port 1. Hotplug through `XGetDeviceChanges`.
- "Modern controls" needed a hook on the game's camera update (eye/look/up setters), not a remap, and user feedback changed the design twice. Measure movement speeds before changing them, and expect the original controls to have quirks that players want kept as an option.

## Testing and debugging
- **Environment-variable switches** for everything: kernel trace, screenshots, auto-press, frame A/B dumps, per-shader overrides, draw logs, texture and shader dumps, a frame-time profiler.
- **Scripted input** (`REWHACKED_INPUT='<s>[@port]:button,...'`) made menus, split screen and LAN reproducible without a human. Build it early here.
- Pre-transformed draws, first-calls-of-each-kind "survey" log lines and a draw list per frame found most rendering bugs.
- The user plays and reports; fix what they find. A known-bugs page keeps open issues and their theories honest (one audio theory turned out wrong).
- Bug reports from testers come back as `loader.log`.

## Tooling and process
- Ghidra MCP holds the project lock: bulk analysis uses a separate work project; disassembly in a pinch with Capstone straight from the XBE.
- Hash the disc and XBE first; HLE patches check the XBE SHA-1 before patching (all addresses belong to one exact build).
- Upscaling (local ComfyUI): Real-ESRGAN anime 6B is fast for UI; SeedVR2 7B sharp keeps more detail; ComfyUI's `--fast` gives NaN (black) frames with SeedVR2. Validate downloaded models (official source, size/hash, static pickle scan, safetensors preferred). Packs are generated locally and never shipped.
- Private alpha builds go to a git-ignored `alpha/` folder via a script, with a player guide; never put game files in them.
- Before any public release: legal review, licence file, remove personal paths, review git history, check Xbox Labs.

## What is different about JSRF
- **XDK 4134, not 4831:** every library address, D3D state-array layout and possibly resource-header details change. XbSymbolDatabase (via ghidra-xbe) should name the XDK 4134 functions; verify against the census.
- **No network libraries**, so no XNet/LAN work (ReWhacked's `net/` is dropped). Local split screen (1P–4P assets) is the multiplayer to support.
- **No Bink**: there are no FMV files on the disc, so cutscenes are probably real-time.
- **PAL** disc: 50/60 Hz and language selection (six language variants of many assets).
- Japanese-developed console game: expect frame-locked logic (a higher frame rate may need real work), and cel-shading with outlines that leans on specific shader or render-state behaviour.

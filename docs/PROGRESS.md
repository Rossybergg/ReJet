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

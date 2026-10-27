# Progress

## 2026-10-27 — Phase 0: project set up

- Repo created (ReJet, in `Development\JSRF`). Generic tools copied from ReWhacked and renamed: `xiso.py`, `xbeinfo.py`, `doctor.py`, the Ghidra headless scripts, the inventory scripts, `d3d_abi.py`, `vsh_decode.py`, `gen_kernel.py`, `include/xbox/`. The loader itself is copied in Phase 2, once JSRF addresses are known.
- Disc and XBE hashed (`config/`). `orig/Default.xbe` extracted. PAL, title 49470018 v1, XDK 4134, no XNET/XONLINE, CRI ADX audio in `.text`, no movie files. Details in CLAUDE.md.
- Lessons from ReWhacked: `docs/REWHACKED_LESSONS.md`.
- User decisions: copy and adapt ReWhacked's code (no shared library); stay independent of the existing NA-disc JSRF decompilation.

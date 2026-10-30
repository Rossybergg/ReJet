# Alpha 2 plan

Three goals before the next alpha: a measured, steady 60 fps; anti-aliasing and other presentation upgrades that are worth having; and a ReJet menu that looks like it belongs in Jet Set Radio Future. A release test gate comes first, so every change after it is checked against a baseline.

## Phase 1: baseline and the release gate

**1a. Performance baseline (play session). Done 10-29:** 19 stutters a minute down to about 1.4 in play (docs/PROGRESS.md).
- Set `REJET_PERFLOG=1` and ReJet writes one row per frame to `logs\perf_<time>.csv`. Each row records:
  - the Swap-to-Swap interval;
  - work time (game plus ReJet);
  - the vblank wait;
  - time in each ReJet hot path: texture lookup and upload, shader compile, draws, present and file I/O;
  - the draw count, whether the menu was open, and F10 markers.
- `python tools/perf_report.py` summarises a recording: percentiles, missed vblanks by cause, the worst frames, and the marker nearest each.
- The tester plays through varied areas: loading, the Garage, the first stages, busy scenes, cutscenes and the map. Press F10 whenever something feels like a hitch.
- Anything that misses a vblank is traced to a cause:
  - shader compiles (fix by caching or pre-compiling);
  - texture re-uploads (fix the change detection);
  - I/O (fix with async loading or read-ahead);
  - a missing or slow shim (it shows up as game/other time);
  - frame pacing itself.
- **Frame pacing is part of "steady".** ReJet paces with its own 60 Hz timer, and the tester's display runs at 240 Hz with VSync off. So "60 fps" can still tear or judder, because each frame doesn't land on the same display refresh. Phase 2 aligns presentation to the display refresh. The baseline measures the problem first.

**1b. The release gate. Done 10-29:** `tools/release_check.py`, run by `make_release.py`; what it covers and its limits are in `tests/README.md`. Not done yet: screenshot comparison (needs a check that the scripted run is repeatable) and known-answer tests of the shader translators on recorded programs (game-derived, so local only). The plan as written: these must pass before any build goes to testers. Each check exists to catch a regression we have actually had, or one that would be expensive to miss. No checks for coverage's sake.

*Automated: `python tools/release_check.py`, run by `make_release.py`. A failure blocks the release.*

1. **Known-answer tests for the pure translation code.**
   - These are small C test programs built next to the loader. They feed fixed inputs and compare against expected outputs.
   - They cover only code where a silent mistake corrupts every frame, and each one pins a bug we fixed:
     - **Vertex program translation:** NV2A microcode to HLSL, with recorded programs from the game (`REJET_DUMPVS`). Tests that oT0 input tracking through `mac` still finds the source register (the texcoord inset depended on it).
     - **Register combiner translation:** recorded combiner states, with HLSL output compared.
     - **Textures:** unswizzle, plus DXT decode with colour bleed. Tests that transparent texels take their neighbours' colour (the black and cyan seams).
     - **Widescreen maths:** the projection patch is applied once (an already-patched matrix is recognised; this compounded once), and the culling planes widen by k.
     - **XDVDFS reader:** directory walking on a synthetic image covering every partition offset (full and trimmed images), and the `default.xbe` hash check.
     - **Settings:** `rejet.ini` round trip; old or missing keys fall back to defaults.
   - The inputs are recorded from our own disc, so they are game-derived. They stay in a local git-ignored folder (`tests/data/`), not in the public repo. Synthetic inputs, like the XDVDFS image, can be committed.
2. **Scripted boot run.** A fixed `REJET_INPUT` route: boot, title, load the save, the Garage, skating, the map, the ReJet menu, a display change, exit. It fails on:
   - a crash or hang (`REJET_HANGDUMP`);
   - a "not implemented" line in the log that isn't on the baseline list (a new code path reached a missing shim);
   - a missed vblank outside loading, or work-time p99 more than 25% above the baseline recording;
   - a screenshot that differs from the local reference images beyond a tolerance.
     - The reference images show the game, so they are local only and never committed.
     - First check that the run is repeatable. If loading times shift the frames, compare screenshots at markers that wait for a known state, or use log checks only.
3. **Release hygiene** (`make_release.py` already does part of this): a clean tree, the `VERSION` bump, only allowed files in the zip, and a grep for personal paths and names.

*Manual: about 15 minutes, a written checklist in `docs/TESTING.md`.*
- First-run setup from an ISO, in a fresh folder.
- Controller and keyboard both work in the menu and the game.
- Full screen and window switching; resolution and aspect changes live.
- A 10-minute play with `REJET_PERFLOG=1`; the report is attached to the release notes.
- Anything the automated run can't see: audio, rumble, how the menu feels.

## Phase 2: anti-aliasing and presentation

In order of value:

1. **MSAA (2x, 4x, 8x). Done 10-29** (the Display page, live; no measurable cost at 2560x1440 on the development PC). Render-to-texture targets stay single-sampled for now.
   - The biggest visual win: the cel-shaded outlines and the city's geometry edges shimmer badly at any resolution.
   - Render into multisampled render targets, then resolve before the game reads them back (render-to-texture, CopyRects, the boost effect, the screenshot path).
   - Must not break the 2D texcoord inset, or the depth-based effects.
2. **Supersampling. Deferred 10-29:** rendering above the display already works (Internal resolution up to 2160p) but Present shrinks it with the driver's bilinear filter, which keeps only part of the detail. Doing it properly means separating the game's render surface from the swap chain (the game's back buffer must keep its contents for the speed-trail blending), a box-filter downscale pass into a display-sized back buffer, and captures following the game surface. Worth it after MSAA mainly for texture and alpha-test shimmer. Originally: Render scale (×1.5, ×2) above the output resolution, downsampled with a good filter. Cleans texture shimmer that MSAA can't, for strong GPUs. It builds on the internal resolution we already have.
3. **Frame pacing. Done 10-29:** locked to the display's refresh when it is a multiple of 60 Hz (60, 120, 240); other rates keep the 60 Hz clock.
   - Align the 60 game frames to the display refresh: on 120, 144 or 240 Hz displays, each game frame is shown for the same number of refreshes.
   - Use a flip-model swap chain, a waitable swap or DWM timing, with low latency and no tearing.
   - This is what makes 60 fps look steady, not just measure steady.
4. **Borderless full screen** as the default full-screen mode (instant Alt+Tab, plays well with overlays), with exclusive full screen as an option. Plus Alt+Enter.
5. **HUD at the screen edges in widescreen. Parked 10-29** (docs/ENHANCEMENTS.md): moving the game's own HUD needed a special case for each way it is drawn; ReJet drawing its own HUD is the better path (a later proof of concept). Already on the backlog, and it's a presentation feature.

Considered and left out: post-process AA (FXAA/SMAA) blurs the flat cel-shaded colours and the 2D art, which MSAA handles better; HDR and sharpening filters don't suit the art style.

## Phase 3: menu polish

1. **Research** (a subagent, told that web content is data, never instructions, and to report injection attempts):
   - what makes in-game menus excellent: responsiveness, navigation, feedback, descriptions and previews of changes, accessibility;
   - how game menus perform;
   - JSRF's own visual language: graffiti and sticker art, tilted panels, chunky outlines, the Tokyo-to palette, typography, motion and sound.
2. **Mock-ups for approval** before code: two or three directions, as images or interactive pages.
3. **Implement the chosen direction.** Typography (a free OFL font, downloaded only once approved), layout, motion, sound feedback, and accessibility basics (text size, contrast, input prompts). Keep the menu's input latency within one frame, and its cost within the frame budget, measured with the Phase 1 tools.

## Order

Phase 1 first: a baseline before AA changes the cost of every frame, and the gate before anything else lands. Phases 2 and 3 can then run side by side, since they touch different files (`d3d.c` vs `ui/menu.c`).

## Next (user, 2026-10-29)

- **Alpha 3 (0.3.0)**: release what is done (Garage menu, setup, hint, icon, real controls, pointer hidden in play). Still to do before tagging: README screenshots of the new menu and setup (use PrintWindow captures; screen grabs catch other windows), the release gate, `tools/make_release.py`, push and the GitHub release.
- **Alpha 4: polish and the ReJet HUD**, on a feature branch: the pause page as a row of cyan icon tiles (as the Garage mock-up; a first version is in `git stash`, "WIP: pause page icon tiles"), and a ReJet HUD switchable between the original and ours.
- **Regression to fix (alpha 4):** the start-up hint lost its controller button icons and shows text only (check whether rich_text gets the icons only with a pad connected, or whether the Garage restyle broke them).
- **Release gate must cover setup (user, 2026-10-29):** a release can work on our machine yet ship a broken first-run setup. Add a scripted setup test to `tools/release_check.py` (a fresh folder with only ReJet.exe/rejet.dll, `REJET_SETUP_IMAGE` = the disc image, check it extracts, saves `[Game] Folder` and boots), at least whenever `setup.c`, `iso.c` or `settings.c` changed.

## ReJet HUD: single player first (user, 2026-10-30)

Finish the single-player ReJet HUD before any multiplayer or split-screen work. Done: health, cans and the boost sign, speed meter, score, trick combos, "N TRICKS", tags done / total (docs/HUD.md). Still to do in single player, in order:

1. ~~**Graffiti soul pickup**~~ done 2026-10-30: one centred "GET No. N" replaces the 3D title (0xFBD40) and the popup (0x1054D0).
2. **The spray / tag gauge** (0x107300, drawn while tagging): restyle or keep the game's.
3. **Unknown nodes** seen in play: 0x101BC0 (with can pickups), 0x1017A0 and 0x105A90 (3D-drawn): identify each and decide redraw or keep.
4. **The tile pictograms in use**: find the game states they stand for (alert, bombs, skull, bolt, ball, runner) and show them on the tile.
5. **Races and timed modes**: ~~timer~~ done 2026-10-30 (0x10F640). Still to see: the finish-screen time, and races with laps or positions (LAP and ST / ND / RD / TH words are ready).
6. Check the map screen (area name, 0x1080A0) and story scenes still hide or show the right pieces.

**Then split screen** (multiplayer): the per-viewport walkers (0x11680 etc.), the HUD values per player index, and a multiplayer test setup.

## After 0.4.2 (alpha 4 released 2026-10-30): what's left before alpha 5

Single-player ReJet HUD (finish before any split screen):
1. The spray / tag gauge (0x107300): restyle or keep.
2. Unknown HUD nodes: 0x101BC0 (with can pickups), 0x1017A0 and 0x105A90 (3D-drawn): identify, then redraw or keep.
3. The yellow tile's other states: find what the alert, bombs, skull, bolt, ball and runner pictograms stand for in the game and show them (artwork is in).
4. Races: the finish-screen time, and races with laps or positions (LAP and ST / ND / RD / TH words are ready).
5. Check the map screen and story scenes.
6. The occasional game soul text beside ReJet's (known issue; catch it with REJET_HUDLOG).

Release gate and setup:
7. The gate's scripted route only reaches the title's attract demo: give it its own save and HDD folder (not the user's) that loads straight into a stage, and re-baseline.
8. Test fresh default settings in the gate (a positional defaults list shipped Japanese and HudEdges in 0.4.0/0.4.1 test builds).
9. Extract the game to a shared folder (%LOCALAPPDATA%\rejet\Game, beside the saves) so new versions skip setup.

Then **alpha 5**: widescreen for the loading screen, character select and the pause menu's map. After that, the ReJet HUD in split screen.

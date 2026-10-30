# Known bugs

Seen but parked until the main features work. Newest first; move an entry to `PROGRESS.md` when it is fixed.

| Seen | Where | What | Notes |
|---|---|---|---|
| 2026-10-30 | ReJet HUD, graffiti soul pickup | Sometimes the game's own soul text shows alongside ReJet's "GET No. N" | Intermittent: the logs show the game's soul title (0xFBD40) and popup (0x1054D0) skipped on every run, and no other node drawing. Next time: run with `REJET_HUDLOG=1` and check the "soul-time node" lines, plus a snip of the text (docs/HUD.md). |
| 2026-10-30 | ReJet HUD, split screen | Not adapted to local multiplayer yet: it reads player 0 and draws for the whole screen | Parked on purpose (user): finish the single-player HUD first (docs/ALPHA2_PLAN.md), then the per-viewport walkers (0x11680 etc.) and per-player values. Use the Classic HUD in split screen. |
| 2026-10-29 | Player (character) select screen | Renders incorrectly | User report, no details or capture yet: press F11 on that screen. The screen draws each character into render targets (captured 10-29: vertex programs 5, 14, 20 and 29 into a 1024x768 target, with combiners 0 and 3). Check after the 10-29 pixel-shader constant fix, which changed every combiner draw. |
| 2026-10-29 | Entering a new area | One frame of about 0.25 s as the new scene's textures are uploaded on first use (was 0.45 s) | Upload them while the loading screen is up instead: hook D3D's `Register` (0x1934A0) or pre-upload during idle loading frames. |
| 2026-10-28 | 2D text in batches | Quads drawn as one indexed batch (e.g. 24 indices) don't get the half-texel inset yet, so a faint line may remain along some text above 640x480 | `inset_quad_texcoords` handles single quads and non-indexed batches. |
| 2026-10-28 | ReJet menu | F12 doesn't take a screenshot while the menu is open (the game is paused in Swap) | Take it from the menu loop. |
| 2026-10-28 | Intro fly-through, grind rails | Shimmering: see-through black patches flicker over the rails as the camera moves | User report. Possibly the stencil shadow volumes (the shadow quad darkens where the stencil count is off: z-fighting between volume and rail, or volume caps). Capture with F11 when it happens. |
| 2026-10-28 | Intro fly-through (frame ~2400) | A large flat white shape covers the lower part of the screen | Not a regression (same with the 10-27 build). Check against the original: may be foreground geometry missing its texture or lighting. |
| 2026-10-27 | Garage | No ground shadow under the player | No character shadow volume is drawn there; find how the character shadow is made. |

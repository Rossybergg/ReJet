# Known bugs

Seen but parked until the main features work. Newest first; move an entry to `PROGRESS.md` when it is fixed.

| Seen | Where | What | Notes |
|---|---|---|---|
| 2026-10-29 | Spraying graffiti | The sprayed tag doesn't appear on the wall (the tag counter goes up) | User report; still there in the 10-29 play session (captures at frames 8205 and 9733). Not ZBias (now implemented, but the game only sets 0). No tag-like draw found in the captures yet; the draw log shows stage 0's texture only. Next: log every stage's texture, capture while spraying at a tag spot, and look for the tag draw (or why it is skipped). |
| 2026-10-29 | Entering a new area | One frame of about 0.25 s as the new scene's textures are uploaded on first use (was 0.45 s) | Upload them while the loading screen is up instead: hook D3D's `Register` (0x1934A0) or pre-upload during idle loading frames. |
| 2026-10-28 | 2D text in batches | Quads drawn as one indexed batch (e.g. 24 indices) don't get the half-texel inset yet, so a faint line may remain along some text above 640x480 | `inset_quad_texcoords` handles single quads and non-indexed batches. |
| 2026-10-28 | ReJet menu | F12 doesn't take a screenshot while the menu is open (the game is paused in Swap) | Take it from the menu loop. |
| 2026-10-28 | Intro fly-through, grind rails | Shimmering: see-through black patches flicker over the rails as the camera moves | User report. Possibly the stencil shadow volumes (the shadow quad darkens where the stencil count is off: z-fighting between volume and rail, or volume caps). Capture with F11 when it happens. |
| 2026-10-28 | Intro fly-through (frame ~2400) | A large flat white shape covers the lower part of the screen | Not a regression (same with the 10-27 build). Check against the original: may be foreground geometry missing its texture or lighting. |
| 2026-10-27 | Garage | No ground shadow under the player | No character shadow volume is drawn there; find how the character shadow is made. |

# High frame rates (120 fps and above): investigation

Question: can ReJet run Jet Set Radio Future above 60 fps, and if not directly, can rendering be separated from a 60 Hz game simulation? Short answer: **the game itself can't run faster than 60 without running fast; a smooth high-refresh mode would need ReJet to interpolate frames between the game's 60 Hz updates, which is possible in principle but a research project with visible risks.** Recommendation at the end.

## How the game keeps time (from the code)

- **One update per frame.** The game's per-frame function (`0x123B0`) runs update and draw once per presented frame. It reads the high-resolution timer only for a profiler; the other timer uses are file-load timeouts. **No delta-time** (time since the last frame) is used anywhere found: every movement, animation step and timer in the game advances by a fixed amount per frame, assuming 60 per second (the PAL disc in its 60 Hz mode; at 50 Hz the game simply runs at 5/6 speed, which is why ReJet always uses 60).
- **Frames are paced by the vertical blank.** The game presents once per vblank (presentation interval ONE). ReJet emulates the vblank at exactly 60 Hz on its own timer, independent of the monitor; that's why the game runs at the right speed on any display.
- **Music and sound run on their own thread.** CRI ADX's server runs from a thread that waits on the vblank (`0x13BC70`), counts vblanks and streams audio. It depends on the vblank ticking at 60 Hz, not on how often frames are drawn. Music isn't synchronised to gameplay events in a way that depends on the frame rate.
- **Input** is read once per game update.

## Answers to the questions

1. **Tied to 60 fps:** all game logic: player and character movement, physics and collisions, tricks and grinds, animations (skeletal and texture), the camera, particles and effects, AI and police, timers, 2D animation and menus, input. In short, everything the game simulates.
2. **Can safely run above 60:** only what ReJet adds on top: its own menu and overlays, and presenting the same frame more often (which looks identical). Nothing in the game's own output changes faster than its 60 Hz updates.
3. **Decoupling rendering from logic:** the game has no such separation (update and draw are one function, with no stored previous state to blend from). ReJet could add one from the outside: record what the game draws each frame (every draw call with its transform matrices and vertex-program constants), and for each in-between display frame, redraw the last frame with transforms **interpolated** between the previous and the current game frame. That is "draw-call interpolation", done by some modern ports of fixed-rate games.
4. **Risks of that approach:**
   - **Matching objects between frames:** a draw must be paired with the same object in the next frame (by vertex data, textures and order). The game's draw lists change as objects appear, disappear and change level of detail; mismatches cause flicker or objects jumping.
   - **Things that can't be interpolated:** particles, graffiti spray and other geometry the game rebuilds on the CPU each frame; 2D and HUD (should stay as they are); texture animations; the shadow volumes (stencil) and render-to-texture effects such as the boost blur.
   - **Camera cuts and teleports** (cutscenes, respawns) must be detected, or the picture smears across the cut.
   - **Latency:** interpolating between frame N-1 and N means showing the game about one 60 Hz frame (16.7 ms) late.
   - **Cost:** every frame is drawn 2x or more (at 120/240 Hz), on top of the game; fine on modern GPUs, but ReJet's D3D9 path isn't optimised for it.
   - **Gameplay, physics, timing and music are not at risk** with this approach, because the game still runs at exactly 60 Hz; the risks are visual only.
5. **A high-refresh mode with a 60 Hz simulation is feasible** in that form, as an experimental option. Running the simulation itself faster is not: without delta time, the game would play at double speed.

## Recommendation

- **Now (alpha):** keep the locked 60 Hz simulation, which is correct. Make 60 fps look its best on high-refresh monitors: VRR (G-Sync / FreeSync) in borderless full screen shows 60 fps evenly; without VRR, 60 on a 144 Hz screen judders slightly, so suggest 120 Hz or VRR in the readme, and keep ReJet's frame pacing tight (it already paces on its own timer).
- **Players who want more today** can use external frame generation that works on any window (for example Lossless Scaling); it doesn't need anything from ReJet.
- **Later (research, behind an "Experimental: smooth motion" switch):** prototype draw-call interpolation for the 3D scene only (camera, characters, world objects), drawing particles, effects and 2D unchanged, with camera-cut detection. Start with the camera alone, which gives most of the perceived smoothness when skating (a large part of what moves on screen is the view). Estimate: weeks of work, and quality depends on how cleanly draws can be matched; worth a time-boxed spike once the alpha is stable.

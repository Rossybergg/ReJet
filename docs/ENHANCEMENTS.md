# Enhancements

What ReJet adds to the original game, done and planned. Every enhancement is a setting in the in-game ReJet menu (F1, or Back + Start on a controller), saved to `rejet.ini`, with the original behaviour available. Ideas are ordered roughly by value for the effort; each says what it needs.

## Done

| Enhancement | Notes |
|---|---|
| ReJet menu | In-game pause menu in the JSRF style with the ReJet logo and animations (slide in/out, staggered rows, gliding selection); controller and keyboard; settings saved at once. |
| Bug reports | Menu → Report a bug: a capture of the moment plus a category and typed note (`note.txt`). F10/F11/F12 for markers, captures, screenshots, with on-screen confirmations. |
| Any resolution | 480 to 2160 lines in the chosen aspect, live (device reset). VSync live too. |
| Widescreen (Hor+) | 16:10, 16:9, 21:9, live: the game's projection and its culling are widened (more visible at the sides, same vertical view, no pop-in at the old edges); 2D keeps 4:3 proportions in the middle (full-screen 2D such as fades and cutscene bars spans the screen). |
| First-run setup | No launcher needed: with no game files found, ReJet asks for the disc image (checked, extracted to `Game\` with progress) or an extracted folder, and remembers it; the ReJet menu can ask again. |
| Cleaner 2D at high resolution | No dark seams around 2D art (DXT transparent texels get their neighbours' colour) and no coloured lines along text boxes (quad texture coordinates pulled in by half a texel). |
| Full screen, window size | Borderless full screen, and a window size independent of the resolution, live. DPI aware (sharp on scaled displays). |
| Texture filtering | Anisotropic 2x-16x, live. |
| FPS counter | Live. |
| Volume, mute in background | Live; music is lowered while the menu is open. |
| Always 60 Hz | The PAL disc runs as PAL60; 50 Hz is gone. |

## Next (Phase 4)

| Idea | Why | What it needs |
|---|---|---|
| MSAA | Smoother edges. | A multisampled scene buffer resolved into the back buffer (and back to the DISCARD swap effect); the device reset path exists now. |
| HUD anchored to the screen edges in widescreen | The HUD now sits in the middle 4:3 area. | Per element: move edge-anchored HUD pieces (health bar, can counter) outwards; needs the HUD's draw sites. |
| Right-stick camera | The original camera only recentres (L trigger); a right-stick orbit is standard now. | Hook the camera update (docs/SUBSYSTEMS.md); ReWhacked's orbit camera is the model. |
| Button remapping | Players expect it on PC; keyboard layout is fixed now. | A Controls page that edits bindings, saved to `rejet.ini`. |
| Custom soundtrack | The Xbox let JSRF play music ripped to the hard disk. | Map a Windows folder of music to the Xbox soundtrack API (XAPI soundtracks, DSound/WMA decode); check the game's use first. |
| Skip intro logos | Faster start-up. | Find the boot sequence's logo states. |
| Controller prompts, rumble switch | Xbox 360/One button names match; rumble on/off. | Small: XInput vibration is already passed through. |

## Later / ideas

- **HD texture packs** (local ComfyUI pipeline, never distributed), the same as ReWhacked's plan.
- **Higher frame rate** (120/144 Hz) only if game logic can be separated from rendering (it looks frame-locked).
- **Split screen with PC controllers**: JSRF has 2-4 player modes; XInput ×4 is already mapped.
- **Graphics extras**: sharper shadows (stencil shadows are resolution-independent already), optional bloom-free/cleaner look, LOD distance.
- **Quality of life**: pause when the window loses focus, quick restart of a level, speedrun timer overlay.
- **Launcher for first-time setup**: for people pulling the public repo: find their disc image, check its hash, extract it. Settings stay in the in-game menu.

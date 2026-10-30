# Enhancements

What ReJet adds to the original game, done and planned. Every enhancement is a setting in the in-game ReJet menu (F1, or Back + Start on a controller), saved to `rejet.ini`, with the original behaviour available. Ideas are ordered roughly by value for the effort; each says what it needs.

## Done

| Enhancement | Notes |
|---|---|
| ReJet menu | In-game pause menu in the JSRF style with the ReJet logo and animations (slide in/out, staggered rows, gliding selection); controller and keyboard, with Xbox button icons when a controller is connected; settings saved at once. |
| Presentation | ReJet icon (from the logo's R), clean window title, a start-up splash with the logo that fades into the game's first logo. |
| Bug reports | Menu → Report a bug: a capture of the moment plus a category and typed note (`note.txt`). F10/F11/F12 for markers, captures, screenshots, with on-screen confirmations. |
| Any resolution | 480 to 2160 lines in the chosen aspect, live (device reset). VSync live too. |
| Widescreen (Hor+) | 16:10, 16:9, 21:9, live: the game's projection and its culling are widened (more visible at the sides, same vertical view, no pop-in at the old edges); 2D keeps 4:3 proportions in the middle (full-screen 2D such as fades and cutscene bars spans the screen). The front-end and pause screens (Now Loading, the load screen, character select, the map, the intro) are 3D sets built for 4:3, so they're shown as the original picture stretched to fill instead. |
| First-run setup | No launcher needed: with no game files found, ReJet asks for the disc image (checked, extracted to `Game\` with progress) or an extracted folder, and remembers it; the ReJet menu can ask again. |
| Cleaner 2D at high resolution | No dark seams around 2D art (DXT transparent texels get their neighbours' colour) and no coloured lines along text boxes (quad texture coordinates pulled in by half a texel). |
| Full screen, window size | Borderless full screen, and a window size independent of the resolution, live. DPI aware (sharp on scaled displays). |
| Texture filtering | Anisotropic 2x-16x, live. |
| FPS counter | Live. |
| Volume, mute in background | Live; music is lowered while the menu is open. |
| Always 60 Hz | The PAL disc runs as PAL60; 50 Hz is gone. |
| Anti-aliasing (MSAA 2x, 4x, 8x) | Smooth edges on outlines and geometry; Display page, live, off by default. The game draws into a multisampled colour and depth surface, resolved into the back buffer at Swap and before screen copies (d3d.c, `msaa_*`); render-to-texture targets stay single-sampled. |

## Next (Phase 4)

| Idea | Why | What it needs |
|---|---|---|
| Boost trails (off by default, the user's choice 10-30) | While boosting the game draws the scene into a 512x512 and a 128x128 target on alternate frames, always shows the 512x512 at half opacity over the last frame, and lays a blur from the 128x128 over that: the picture moves at 30 fps and smears (it looked like half-speed animation). Off: the 128x128 frames are drawn through the 512x512 target and shrunk into the small one, the composite is opaque and the blur overlay is skipped. On: as the game does it. | Done (d3d.c, `boost_big_target`, `boost_composite`). |
| ReJet's own HUD (proof of concept, later) | The game's HUD is 640x480 2D art: soft at high resolutions and laid out for 4:3. Drawn by ReJet, like its menu, it could be sharp at any resolution and laid out for any aspect (the user's idea, 10-29). | The game values it shows (health, can count, tag counter, boost), found in memory (`REJET_MEMDUMP`, `tools/memdiff.py`); the original HUD hidden (its draws are recognisable by the HUD layer at 0x1367B); a design to match JSRF's style. |
| HUD anchored to the screen edges in widescreen (dropped) | The HUD sits in the middle 4:3 area. | Tried: HUD draws are recognised by the game code above them (the HUD layer at 0x1367B on the stack) and squeezed around the right edge, with 3D pieces shifted. The game draws its HUD several ways (screen-space quads, orthographic and perspective 3D, vertex programs, and the trick display partly through the sprite list cutscenes share), and each needed its own fix; centre pieces stretched. Dropped in favour of ReJet's own HUD (next row); the code was removed in 0.4.0. |
| Right-stick camera | The original camera only recentres (L trigger); a right-stick orbit is standard now. | Hook the camera update (docs/SUBSYSTEMS.md); ReWhacked's orbit camera is the model. |
| Button remapping | Players expect it on PC; keyboard layout is fixed now. | A Controls page that edits bindings, saved to `rejet.ini`. |
| Custom soundtrack | The Xbox let JSRF play music ripped to the hard disk. | Map a Windows folder of music to the Xbox soundtrack API (XAPI soundtracks, DSound/WMA decode); check the game's use first. |
| Skip intro logos | Faster start-up. | Find the boot sequence's logo states. |
| Controller prompts, rumble switch | Xbox 360/One button names match; rumble on/off. | Small: XInput vibration is already passed through. |

## Later / ideas

- **HD texture packs** (local ComfyUI pipeline, never distributed), the same as ReWhacked's plan.
- **Smooth motion above 60 fps** (experimental): the game's logic is locked to 60, so this would mean interpolating frames; see [HIGH_FRAME_RATE.md](HIGH_FRAME_RATE.md).
- **Split screen with PC controllers**: JSRF has 2-4 player modes; XInput ×4 is already mapped.
- **Graphics extras**: sharper shadows (stencil shadows are resolution-independent already), optional bloom-free/cleaner look, LOD distance.
- **Quality of life**: pause when the window loses focus, quick restart of a level, speedrun timer overlay.

# The HUD: research for a ReJet HUD

Goal (alpha 4): a menu setting **HUD: Classic / ReJet**. Classic is the game's own HUD, unchanged, in HD. ReJet draws our own HUD in the Garage style (the "Garage HUD" design board), close to the original's look (cel-shaded, not comic-like), with small improvements such as how many tags are left in the stage. It has to work in single player first, then in split screen.

That needs two things: a way to leave out the original HUD, and the values it shows.

## How the game draws its HUD (found 2026-10-29)

- The 2D overlay is a tree of display nodes. `FUN_00011220` walks it: children at node + 0x28, siblings at + 0x30. For each node whose flags (+4) lack 0x40000, it pushes the matrix stack (0x1BB480), calls the node's draw method (vtable slot 3, thiscall, one argument 0) and pops it (0x1BB510).
- It is reached from the object list in `FUN_000131c0` through `FUN_00012650` (single player: straight to 0x11220; split screen: 0x11680 / 0x11450 / 0x118B0 / 0x11AE0, one per viewport flag at object + 0x40..0x4C, still to look at).
- The HUD's draw methods live in one block of game code, 0x100000-0x10F000. In single-player gameplay these node draw methods run (`REJET_HUDHIDE` logs them as `hud: node draw`):

| Draw method | Vtable | Draws (found by hiding one at a time, `REJET_HUDHIDE=cycle`) |
|---|---|---|
| 0x106AD0 | 0x1D5D60 | the health bar |
| 0x10E680 | 0x1D6138 | the can icon and the can count |
| 0x10DEA0 | 0x1D60F0 | the speed meter (bottom right) |
| 0x1080A0 | 0x1D5DE8 | the can icon, and the area name on the map screen (a shared sprite class) |
| 0x105A90 | 0x1D5D20 | nothing visible while skating; not seen after the first minutes of play |
| 0x10A590 | 0x1D5E28 | the **boost icon** (the yellow skater sign shown from 10 cans); confirmed in play 2026-10-30 |
| 0x03CA80, 0x07E2D0, 0x0BB4A0, 0x046540, 0x0F8030, 0x0FE6E0, 0x0F8A40, 0x0F8810 | | other 2D (title, map, effects) |

## Leaving out the original HUD

`loader/core/ui/hud.c` replaces `FUN_00011220` with the same walk in C (an HLE patch), so ReJet can skip chosen nodes. With `REJET_HUDHIDE=1` (a test switch for now) it skips every draw method in the HUD module:

- In gameplay the health bar, the can icon and count and the boost meter disappear; the 3D scene is untouched.
- On the map screen everything stays except the **area name** ("Garage" and its dotted bar), drawn by 0x1080A0, the same class as the can icon. So the setting skips a list of methods (health, cans, boost) and must leave 0x1080A0 alone while the map is open (for example: skip it only in frames where the HUD's other nodes draw).

## The values, read from the HUD's own nodes

The walker sees every HUD node, so the ReJet HUD can read what the original would draw from the same nodes, with no hunting for addresses that move between sessions (the can count's address did). `REJET_HUDLOG=1` logs these fields when they change:

| Node (draw method) | Field | Seen | Meaning |
|---|---|---|---|
| health (0x106AD0) | + 0x48 / + 0x60, + 0xA8 / + 0xC0, + 0x108 / + 0x120 | 395 / 585.5, 395 / 516.9, 395 / 516.9 then falling | quads in 640x480 space: the whole bar, this character's maximum, and the **current health** (+0x120 fell 516.9 to 406.1 under hits in play, 2026-10-30); lost health shows black |
| health | + 0x2B0 | 46 | the bar's length in 4-unit segments (0x1CB0B0 = 4.0), not the health |
| health | + 0x2B4 | a handle | the player (`FUN_00012890(handle)` returns the player object; draws while player + 0x16AC < 0) |
| cans (0x10E680) | + 0xB8 | 0 .. 15 in play | cans held (confirmed 2026-10-30: rises as cans are collected, falls when boosting or spraying) |
| cans | + 0xD8 | 30 | the can cap |
| cans | + 0xD0, + 0xD4 | 0 | digit glyphs of the count |
| speed (0x10DEA0) | + 0x1D0 / + 0x1D4 | 0 .. 2.31 | the **speed meter** (not a boost meter: it rises as you skate, full when boosting): current speed (clamped) over the maximum, as its update method 0x10DDA0 computes; that method also shifts the fill from blue to red with speed |

Confirmed in a real play session with `REJET_HUDLOG=1` (2026-10-30): the can count follows collecting and spending cans, and the speed meter reaches exactly 1.0 while boosting.

## The ReJet HUD

Menu: Game > **HUD: Classic / ReJet** (`rejet.ini` [Display] `Hud=0/1`, default Classic). With ReJet, hud.c skips the original health bar (0x106AD0), cans (0x10E680) and speed meter (0x10DEA0), and `draw_rejet_hud` (menu.c) redraws them at the screen's edges, close to the game's shapes but cleaner: flat cel-shaded colour with a light top and shaded bottom, thin dark outlines and soft shadows (the user: modern, not comic-like). The can is red with no cans and yellow with some, as the game's. Each piece shows only when the game would show its own: the draw methods check the player's HUD flags at player + 0x16AC (player = *(manager + 0x98 + index * 4), manager at 0x236580, index in the node: health + 0x2B4, cans + 0x100, speed + 0x1D8), bit 31 for health, 0x4000000 for cans, 0x2000000 for speed. So the ReJet HUD is hidden in story scenes, on the map and in cutscenes, as the original. `REJET_SHOTSOVERLAY=1` makes `REJET_SHOTS` screenshots include ReJet's overlay.

## Score, tricks and tags (found 2026-10-30)

- **The stat getter** FUN_00108F00 (thiscall on the player, one id) feeds the popups. Named from two play sessions with `REJET_HUDLOG=1` (it logs all 17 when one changes): 0 / 1 health max / current, 4 / 5 can cap / cans, **6 the score**, **0xA tricks in the last combo**, 0xB the last graffiti soul picked up (0-based: 33 is soul 34), 0xC boost ready, **0xD / 0xE / 0xF the combo's points, multiplier (1, 2, 4, 8) and running total**. 2 and 3 are floats.
- **Popups** (each update method runs a state machine: slide in, hold 120 frames, fade):
  - 0x1009F0 (update 0x100790): points x multiplier in gold (0xE6C800), then the total; fade + 0x114, slide-in + 0x118, the total's fade + 0x11C; shown while player flag 0x100 is set. Its label is the full-width "X" at 0x1D1560.
  - 0x102940 (update 0x1027E0): "N TRICKS" from stat 0xA; fade + 0x134, slide-in + 0x13C; flag 0x20000000.
  - 0x104AD0 (update 0x1049D0): **"5/16"** when a tag is done; done + 0x48, total + 0x4C, fade + 0x134. Spawned by FUN_00108CA0 kind 1 from FUN_001077B0, which takes the numbers from **FUN_0004DC00** (thiscall on the object behind handle 9; fills a 0x118-byte summary: tags in the stage, then tags done, counted from the stage's tag spots by FUN_0003EEF0 / FUN_0003EF30).
  - 0x1054D0 (update 0x1053B0): **"GET No. N"** for a graffiti soul; the number + 0x11C (its player's stat 0xB + 1), fade 0..255 + 0x130, state + 0x128 (3 hold, 5 fade, 6 done). The pickup's camera spin hides it (flag 0x100000 off) and pauses its timer, so it shows on and off; meanwhile 0xFBD40 (vtable 0x1D58E8, outside the HUD module) draws a 3D title. ReJet skips both and shows one "GET No. N", large in the centre, from the node's first frame until it fades (it stays up while the node was last seen holding).
  - 0x104FF0: the trick's name (text from the language's string tables; flag 0x10000000). ReJet keeps the game's.
- ReJet (hud.c) reads these each frame, skips the three popups in ReJet mode and draws its own on their timing (menu.c): the score at the top left, the combo and "N TRICKS" sliding in below it, and **tags done / total** under the cans, which pops when a tag is done.

## Still to find

- Split screen: the per-viewport walkers (0x11680 etc.) need the same treatment.

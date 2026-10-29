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
| 0x10DEA0 | 0x1D60F0 | the boost meter (bottom right) |
| 0x1080A0 | 0x1D5DE8 | the can icon, and the area name on the map screen (a shared sprite class) |
| 0x105A90 | 0x1D5D20 | nothing visible while skating: probably trick, score or tag text that only shows in some states |
| 0x10A590 | 0x1D5E28 | as above |
| 0x03CA80, 0x07E2D0, 0x0BB4A0, 0x046540, 0x0F8030, 0x0FE6E0, 0x0F8A40, 0x0F8810 | | other 2D (title, map, effects) |

## Leaving out the original HUD

`loader/core/ui/hud.c` replaces `FUN_00011220` with the same walk in C (an HLE patch), so ReJet can skip chosen nodes. With `REJET_HUDHIDE=1` (a test switch for now) it skips every draw method in the HUD module:

- In gameplay the health bar, the can icon and count and the boost meter disappear; the 3D scene is untouched.
- On the map screen everything stays except the **area name** ("Garage" and its dotted bar), drawn by 0x1080A0, the same class as the can icon. So the setting skips a list of methods (health, cans, boost) and must leave 0x1080A0 alone while the map is open (for example: skip it only in frames where the HUD's other nodes draw).

## The values, read from the HUD's own nodes

The walker sees every HUD node, so the ReJet HUD can read what the original would draw from the same nodes, with no hunting for addresses that move between sessions (the can count's address did). `REJET_HUDLOG=1` logs these fields when they change:

| Node (draw method) | Field | Seen | Meaning |
|---|---|---|---|
| health (0x106AD0) | + 0x2B0 | 46 at the start of play | filled health segments |
| health | + 0x2B4 | a handle | the player (`FUN_00012890(handle)` returns the player object; draws while player + 0x16AC < 0) |
| cans (0x10E680) | + 0xB8 | 0 with no cans | cans held (to confirm by collecting some) |
| cans | + 0xD8 | 30 | the can cap |
| cans | + 0xD0, + 0xD4 | 0 | digit glyphs of the count |
| boost (0x10DEA0) | + 0x120, + 0x13C, + 0x138 | 0, then floats (586, 420, 580) | the meter's geometry; the fill amount is computed from tables (0x227330..), still to pin down |

To confirm: a short real play session with `REJET_HUDLOG=1` (collect cans, take a hit, boost) and read `hud:` lines in loader.log.

## Still to find

- The values: health, cans, boost, score, tags left. Each HUD node probably reads them from the player object; the draw methods above are the place to look (their loads give the offsets), which should be more stable than raw addresses (the can count's address moved between sessions).
- Split screen: the per-viewport walkers (0x11680 etc.) need the same treatment.

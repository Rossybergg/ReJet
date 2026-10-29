# The HUD: research for a ReJet HUD

Goal (alpha 4): a menu setting **HUD: Classic / ReJet**. Classic is the game's own HUD, unchanged, in HD. ReJet draws our own HUD in the Garage style (the "Garage HUD" design board), close to the original's look (cel-shaded, not comic-like), with small improvements such as how many tags are left in the stage. It has to work in single player first, then in split screen.

That needs two things: a way to leave out the original HUD, and the values it shows.

## How the game draws its HUD (found 2026-10-29)

- The 2D overlay is a tree of display nodes. `FUN_00011220` walks it: children at node + 0x28, siblings at + 0x30. For each node whose flags (+4) lack 0x40000, it pushes the matrix stack (0x1BB480), calls the node's draw method (vtable slot 3, thiscall, one argument 0) and pops it (0x1BB510).
- It is reached from the object list in `FUN_000131c0` through `FUN_00012650` (single player: straight to 0x11220; split screen: 0x11680 / 0x11450 / 0x118B0 / 0x11AE0, one per viewport flag at object + 0x40..0x4C, still to look at).
- The HUD's draw methods live in one block of game code, 0x100000-0x10F000. In single-player gameplay these node draw methods run (`REJET_HUDHIDE` logs them as `hud: node draw`):

| Draw method | Vtable | Code block |
|---|---|---|
| 0x1080A0 | 0x1D5DE8 | HUD module |
| 0x10DEA0 | 0x1D60F0 | HUD module |
| 0x106AD0 | 0x1D5D60 | HUD module |
| 0x105A90 | 0x1D5D20 | HUD module |
| 0x10E680 | 0x1D6138 | HUD module |
| 0x10A590 | 0x1D5E28 | HUD module |
| 0x03CA80, 0x07E2D0, 0x0BB4A0, 0x046540, 0x0F8030, 0x0FE6E0, 0x0F8A40, 0x0F8810 | | other 2D (title, map, effects) |

## Leaving out the original HUD

`loader/core/ui/hud.c` replaces `FUN_00011220` with the same walk in C (an HLE patch), so ReJet can skip chosen nodes. With `REJET_HUDHIDE=1` (a test switch for now) it skips every draw method in the HUD module:

- In gameplay the health bar, the can icon and count and the boost meter disappear; the 3D scene is untouched.
- On the map screen everything stays except the **area name** ("Garage" and its dotted bar): one of the six HUD-module methods draws that too. So the setting must skip a list of methods, not the whole block (next step: find which one draws the area name).

## Still to find

- Which method draws which element (health, cans, boost, score and trick text, area name), by hiding them one at a time.
- The values: health, cans, boost, score, tags left. Each HUD node probably reads them from the player object; the draw methods above are the place to look (their loads give the offsets), which should be more stable than raw addresses (the can count's address moved between sessions).
- Split screen: the per-viewport walkers (0x11680 etc.) need the same treatment.

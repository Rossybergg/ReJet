# Testing ReJet by hand

How to play-test and report problems so they can be found and fixed quickly. Everything below is written next to the loader, in `build\loader\`.

## Starting the game

- `play.bat 0` runs the game until you close the window (`play.bat` alone stops after 30 seconds).
- Controller: an Xbox-style pad on port 1. Keyboard (when no pad is plugged in): WASD steer, Space or Z = A, X or Esc = B, C = X, V = Y, Q and E = triggers, Enter = Start, Backspace = Back, arrow keys = d-pad.
- Saves are kept in `%LOCALAPPDATA%\rejet\hdd\Partition1\UDATA\49470018\` and survive restarts and rebuilds.

## The ReJet menu

Press **F1**, or **Back + Start** on any controller, to pause the game and open the ReJet menu. Move with the d-pad, left stick or arrow keys; A / Enter selects, B / Esc goes back, Start / F1 resumes. Settings (display, audio, language) are saved at once and kept for the next session; those marked * apply the next time ReJet starts.

**Report a bug** in the menu is the easiest way to report a problem: pick what went wrong, optionally type a note, and save. It makes a bug capture (below) of the game screen with your note in `note.txt`.

## Keys for testing

The window title lists them. Each shows a short confirmation in the top left corner.

| Key | What it does | Where it goes |
|---|---|---|
| **F12** | Screenshot | `Screenshots\jsrf_<date>_<time>.bmp` |
| **F11** | **Bug capture** of the next frame: screenshot, every draw call with its state (and the raw vertices of small draws), screen clears, materials and lights, each off-screen render texture as an image, and a summary with the Xbox render-state array | `captures\capture_NNN_<date>_<time>\` (`screen.bmp`, `draws.txt`, `rendertarget*.bmp`, `info.txt`) |
| **F10** | Marker: writes `MARK 1`, `MARK 2`, ... with the frame number into the log | `loader.log` |

## Reporting a problem

1. When something looks wrong, press **F11** while it is on screen (for a short effect such as the boost, press it during the effect; press it a few times if unsure). Press **F10** just before trying something risky, so the log shows where it started.
2. Say in a sentence what you saw and what you did, for example "boost: screen grey and red, capture 3", or "crashed after mark 2 when I opened the map".
3. Leave the files where they are: the next session reads them from `build\loader\`.

Parked problems are listed in [KNOWN_BUGS.md](KNOWN_BUGS.md).

## Logs

- `loader.log` is the current session. When a new session starts, the previous one is moved to `logs\loader_<date>_<time>.log`, so nothing is lost between sessions.
- If the game stops, the reason is at the end of the log (`stop:` and an `EXCEPTION` or `KERNEL` line with a backtrace).

## Switches for scripted runs (developers)

Set as environment variables before starting `ReJetLoader.exe`:

| Switch | Effect |
|---|---|
| `REJET_INPUT='<s>[@<port>]:<button>,...'` | Timed button presses (0.2 s each), e.g. `36:start,45:a,72:ls_up` |
| `REJET_SHOTS=<n>` | `shot_NNNNN.bmp` every n frames (`tools/contact_sheet.py` makes a labelled sheet) |
| `REJET_CAPTURE_AT=<frame>` | Bug capture of that frame (as F11) |
| `REJET_DRAWLOG=<frame>` | That frame's draws in `loader.log` |
| `REJET_TRACE=1` | Every kernel call in the log |
| `REJET_HANGDUMP=<s>` | Thread dump after s seconds (also written on timeout) |
| `REJET_PERF=<ms>` | Frames slower than ms, with a time breakdown |
| `REJET_AUDIODUMP=1` | The final audio mix to `mix.wav` |
| `REJET_DUMPVS=1` | Vertex programs and declarations to `vsh\` |
| `REJET_INI=<file>` | Another settings file instead of `rejet.ini` |

# Testing ReJet by hand

How to play-test and report problems so they can be found and fixed quickly. Everything below is written next to the loader, in `build\loader\`.

## Starting the game

- `play.bat` runs `build\loader\ReJet.exe` until you close the window. `play.bat <default.xbe> <seconds>` runs a given game for a while (0 = until closed).
- Or run `ReJet.exe` without arguments: it uses the saved game folder (`[Game] Folder` in `rejet.ini`), else `Game\` next to it, else shows the first-run setup. `REJET_SETUP_IMAGE=<iso>` starts the setup with that image (tests).
- Controller: an Xbox-style pad on port 1. Keyboard (when no pad is plugged in): WASD steer, Space or Z = A, X or Esc = B, C = X, V = Y, Q and E = triggers, Enter = Start, Backspace = Back, arrow keys = d-pad.
- Saves are kept in `%LOCALAPPDATA%\rejet\hdd\Partition1\UDATA\49470018\` and survive restarts and rebuilds.

## The ReJet menu

Press **F1**, or **Back + Start** on any controller, to pause the game and open the ReJet menu. Move with the d-pad, left stick or arrow keys; A / Enter selects, B / Esc goes back, Start / F1 resumes. Settings are saved at once and kept for the next session. Resolution sets how sharp the game is drawn, aspect ratio its shape (widescreen shows more at the sides) and window size how big the window is; all change live. Only the language (marked *) waits for the next start.

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

## Before a release: the manual checklist

`python tools/release_check.py` covers crashes, regressions in the tested code, newly reached unimplemented functions and performance (tests/README.md). These need a person, about 15 minutes:

1. **First run:** copy ReJet.exe and rejet.dll into an empty folder, run it, and set up from a disc image. The progress bar finishes and the game starts.
2. **Controller and keyboard:** open the ReJet menu with Back + Start and with F1; move through every page with each.
3. **Display:** change the internal resolution, the aspect and full screen from the menu while playing. The picture is right after each change, with no stretching and the HUD in place.
4. **Play for 10 minutes** with `REJET_PERFLOG=1`: the Garage, a street stage, a cutscene, the map, spraying graffiti. Nothing looks wrong (the known bugs aside), and `python tools/perf_report.py` shows no new stutters.
5. **Sound:** music, effects and voices play; the menu's volume works.
6. **The build string** in the menu (bottom right) matches the release.

## Switches for scripted runs (developers)

Set as environment variables before starting `ReJet.exe`:

| Switch | Effect |
|---|---|
| `REJET_INPUT='<s>[@<port>]:<button>,...'` | Timed button presses (0.2 s each), e.g. `36:start,45:a,72:ls_up` |
| `REJET_SHOTS=<n>` | `shot_NNNNN.bmp` every n frames (`tools/contact_sheet.py` makes a labelled sheet) |
| `REJET_CAPTURE_AT=<frame>` | Bug capture of that frame (as F11) |
| `REJET_DRAWLOG=<frame>` | That frame's draws in `loader.log` |
| `REJET_TRACE=1` | Every kernel call in the log |
| `REJET_HANGDUMP=<s>` | Thread dump after s seconds (also written on timeout) |
| `REJET_PERF=<ms>` | Frames slower than ms, with a time breakdown |
| `REJET_PERFLOG=1` | Every frame to `logs\perf_<time>.csv` (work and wait times, ReJet's hot paths, draws, F10 markers); `python tools/perf_report.py [csv]` summarises it (time on screen per frame, stutters and their causes) |
| `REJET_DUMPHLSL=1` | The HLSL ReJet generates for each vertex program and combiner to `hlsl\vs_NNN.hlsl` / `hlsl\ps_NNN.hlsl` (NNN as in captures' `vs` / `ps` numbers) |
| `REJET_TEXFULL=1` | Texture change checks read every word instead of a sample (to rule out missed CPU writes); texture changes after the first upload are always logged (`tex: changed`) |
| `REJET_PROFILE=1` | Samples the game thread while it works on a frame and logs where the time goes every 30 s (ReJet functions by name from `rejet.pdb`, other modules by name, game code by address) |
| `REJET_AUDIODUMP=1` | The final audio mix to `mix.wav` |
| `REJET_DUMPVS=1` | Vertex programs and declarations to `vsh\` |
| `REJET_INI=<file>` | Another settings file instead of `rejet.ini` |

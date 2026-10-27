# Subsystem map

Findings from Phase 1. Addresses are VAs in `orig/Default.xbe` (PAL, SHA-1 in `config/xbe.sha1`). Decompiles come from `python tools/ghidra/export.py --decompile <list>` (output in `build/`, not committed).

## Layers

Game code (0x11000–0x13B010) never calls Direct3D and calls DirectSound only twice. It talks to an **engine layer** (0x14C25A–0x17CA26) made of COM-style C++ objects: interface GUIDs (for example 0x1E7080), slot 0 QueryInterface, slot 2 Release, constructors that set a vtable and zero their globals. The design, with "Gamepad %ld" device names, DirectInput-style joystick state and a renderer interface with 121 methods, looks like a PC (DirectX 8) engine ported to the Xbox, as Whacked!'s was.

| Global | What |
|---|---|
| `0x25860C` | the game's renderer interface pointer (vtable 0x1E6BA0) |
| `0x26B998` | the engine's own pointer to the same renderer (set in Initialize) |
| `0x25861C` | game-side graphics manager object (0x59C bytes, built in 0x121E0) |

## Renderer (vtable 0x1E6BA0, 121 slots, object 0xE4 bytes)

- Factory `0x14DA70` (malloc 0xE4, constructor `0x14D870`, then QueryInterface with GUID 0x1E7080). Constructor defaults: +0x40 near 0.5, +0x44 far 500.0, +0x48 FOV 0x4000, +0x60 aspect factor 1.0.
- Known slots (index: address):

  | Slot | Address | Role |
  |---|---|---|
  | 6 | 0x155DE0 | Initialize(desc): copies the display descriptor to +0xA8, picks the mode (0x157400) and depth format (0x157490), creates the device (0x157510), sets the default viewport (0x154C40) |
  | 10 | 0x155AB0 | Reset with a new resolution/format (calls 0x157510 with create = 0 → `D3DDevice_Reset`) |
  | 25 | 0x14DAF0 | Clear |
  | 26 / 27 | 0x154D00 / 0x154E80 | GetTransform / SetTransform; the projection (state 1) is cached at 0x26B7B8 (16 floats) |
  | 30 | 0x154890 | SetViewport(rect or full target) → `D3DDevice_SetViewport(0x26B790)`, then slot 31 with the current FOV/near/far |
  | 31 | 0x154A40 | **SetProjection(fov, near, far)**, see below |
  | 42 | 0x14DDF0 | SetRenderTarget |
  | 46 | 0x14DB40 | **Present**: FPS counter (0x155690), then `D3DDevice_Swap(0)` |
  | 47–53 | 0x14DC90–0x14DDC0 | SetVertexShader / DrawVerticesUP / DrawIndexedVerticesUP wrappers |
  | 62 | 0x150F50 | render-state block (every `SetRenderState_*` the engine uses) |
  | 81 | 0x150870 | SetTexture |
  | 119 / 120 | 0x14DBC0 / 0x14DB80 | Present variants (back-buffer readback, `Swap(4)`) |

- Other engine graphics functions: CreateTexture/VolumeTexture `0x1501B0`, vertex/index buffers `0x14DF60`, CreateVertexShader `0x14ECD0`, CreatePixelShader `0x154260`/`0x1542F0`, vertex-shader constant upload `0x14E610` (matrix sets by code: 1–7 = matrices from renderer slot 26 (+0x68), 0x100 = 5 constants from 0x26B608, 0x1000–0x6000 = single matrix rows), lights `0x1504D0`–`0x1506B0`, gamma `0x152BF0`/`0x152C30`.
- Draw entry points: `DrawVertices` 12 wrappers at 0x1565E0..., `DrawIndexedVertices` 4 at 0x156950–0x156A40 (each also calls `SetIndices`), stream source 0x156580.

## Video mode

- The game fills the display descriptor in **`0x5FA70`** (renderer setup, also loads `D:\Media\Font\TNRoman.bin`). Fields (dwords): [0] size 0x38, [1] parameter (window/handle), **[2] width 640, [3] height 480, [4] 16 bpp**, [5] 2 back buffers, [6] multisample 0 → `D3DMULTISAMPLE_NONE` (0x11), [7] 1, [8] 1, **[9] vsync 1**, [10] 500, [11] 0x1000, [12] from 0x14D5B0.
- `0x157400` matches the descriptor against `D3D_EnumAdapterModes`: for 16 bpp it picks format 0x11 (R5G6B5) or 0x1C; for 32 bpp 0x1E (X8R8G8B8). **JSRF renders a 640x480 16-bit back buffer.** Upscaling to a 32-bit host back buffer should help banding.
- `0x157510` builds the Xbox `D3DPRESENT_PARAMETERS` and calls `Direct3D_CreateDevice` (or `D3DDevice_Reset`). PresentationInterval = 1 when [9] is set, else immediate. Flags come from three XAPI queries (see "Console settings" below: `0x147DB8` XGetVideoStandard, `0x147DE1` XGetVideoFlags, `0x147DB0` XGetAVPack): interlaced/progressive for 640x480, and widescreen + progressive at 1280x720 or widescreen + interlaced at 1920x1080 if the dashboard allows them. Refresh is 60 Hz on PAL only with the PAL60 flag (0x40). The game asks for 640x480 only, so the HD branches look like unused engine code.

## Projection and viewport (for widescreen)

- Viewport globals: 0x26B790 x, 0x26B794 y, **0x26B798 width, 0x26B79C height**, 0x26B7A0/0x26B7A4 min/max Z.
- `0x154A40` (slot 31) SetProjection(this, fov, near, far):
  - fov is a **Sega-style angle** (0x10000 = 360°; the default 0x4000 = 90°); `0x14D120` turns it into a scale (+0x54, cot or tan of half the angle);
  - +0x58 = far / (far - near), +0x5C = 1 / far;
  - +0x50 = (viewport width / 2) / scale: the focal length in pixels, from the **width**;
  - screen-space globals at 0x26B7A8–0x26B7B4 (focal length, centre x, -aspect × focal, centre y) for the engine's own transforms;
  - then `0x1544F0` and the matrix builder `0x154920`.
- `0x154920` builds the projection at 0x234F58 and calls `SetTransform(PROJECTION)`. m11 (0x234F6C) = width / (height × aspect factor +0x60). m00 isn't written here (probably by `0x1544F0`, which slot 31 calls just before; to confirm). m22 = +0x58 × +0x54, m23 = +0x54, m32 = +0x4C - m22 × near.
- **Consequence:** the horizontal field of view is fixed and the vertical one follows the viewport shape, so a wider viewport gives **Vert-** (less visible above and below). Hor+ widescreen = hook slot 31 (or the scale +0x54) so the vertical view stays at its 4:3 value and the horizontal one widens. +0x60 (aspect factor, 1.0) is a second knob to check.

## Frame loop and timing

- `0x123B0` is the game's per-frame function (update and draw, chosen by mode flags at +0x40..+0x4C). It reads `QueryPerformanceCounter` at start and end only to feed a profiler (`0x6F160`, totals at +0x87B8/+0x87C0). The other game QPC users (`0x25820`, `0x25910`, `0x262E0`) time async file loads (timeout 15,000,000 counts, 20 ms on the Xbox 733 MHz clock).
- `KeTickCount` is read only by XAPI (0x1481FD, GetTickCount).
- The renderer's FPS counter (`0x155690`) averages over 120 frames (+0x4 frames per second, +0x8 a second rate).
- **No game-side delta timer has been found.** Like many console games of the time, logic probably advances one step per presented frame (vsync, interval ONE). That makes a higher frame rate a real project; confirm at runtime first. On PAL at 50 Hz the game would run at 5/6 speed unless it uses PAL60 or the loader presents at 60.
- The XAPI `QueryPerformanceCounter` at 0x145FF0 must still be HLE'd on a 733.33 MHz clock (ReWhacked lesson): the file-load timeouts and profiler depend on it.
- **ADX vblank thread:** `0x13BC70` / `0x13BCE0` loop on `D3DDevice_BlockUntilVerticalBlank`, count vblanks (0x26586C), run the ADX server (`0x142900`) and a callback (0x265878). The thread is started by `0x13BDE0` with raised priority. The loader's vblank must tick steadily, or music and streams will stutter.
- `D3DDevice_SetVerticalBlankCallback` is called through `0x160480` (from the game manager constructor `0x121E0`, which clears it).

## Input (engine)

- Manager vtable 0x1E7E40 (constructor around 0x167620). Slot 7 `0x1672D0` Initialize: `XInitDevices` once, then 4 pad objects (0x480 bytes each, constructor `0x168840`). Device changes: `0x167470` (`XGetDeviceChanges` / `XGetDevices`).
- Pad vtable 0x1E94C8: slot 25 `0x167C50` **Poll**. It sends rumble with `XInputSetState` when the requested state (+0x47C) changes, calls `XInputPoll` + `XInputGetState`, and writes a DirectInput-style state:
  - sticks at +0x2D4 (lx), +0x2D8 (-ly), +0x2EC (rx), +0x2F0 (-ry), as signed 16-bit values;
  - 8 analog buttons as bytes at +0x304..+0x30B, rescaled from the 30–255 range to 0–255;
  - 4 digital buttons at +0x30C..+0x30F (0x80 when pressed): Start, Back, L3 and R3 (XInput bits 0x10–0x80);
  - the D-pad as a POV angle at +0x2F4 in hundredths of a degree (0 = up, 9000 = right, 18000 = down, 27000 = left, diagonals included, -1 = centred);
  - per-axis dead zones and ranges from 8 tables at +0x374.., previous state copied to +0x324.
- Open `0x1677A0`: `XInputOpen(gamepad, port)`, capabilities, names the device ("Gamepad %ld", "Microsoft Xbox Standard game pad", wheel, arcade stick, ...).
- Game code calls `XGetDevices` directly (9 sites) to check what is plugged in, and `XMountMUA`/`XUnmountMU` for memory units (0x136FF0, 0x137020).

## Sound

- Engine sound object: `0x168F80` creates DirectSound (`DirectSoundCreate`, then downloads the effects image through 0x19EF7C and 0x19EF81), `0x169050` per-frame work (each voice's update, `CommitDeferredSettings`, `DirectSoundDoWork`), 3D voice parameters `0x16DF00` / `0x171BA0` (position, cones, distances, I3DL2), buffers `0x16D2A0` / `0x171820`, effect data `0x16C6E0` / `0x170CC0` / `0x172078`.
- CRI ADX drives its own DirectSound buffers (`0x141E50` creates, `0x1421D0` / `0x1424B0` lock and fill, `0x142280` reads the play position); decoding is ADX code in `.text`, so the loader only needs DSOUND.
- The DSP effects image (reverb, I3DL2) is downloaded at start-up. ReWhacked did not model effects; JSRF may use reverb more (to check).

## Console settings, language and 50/60 Hz

XAPI wrappers around `ExQueryNonVolatileSetting`, named from their setting IDs:

| Address | Function | Setting |
|---|---|---|
| 0x147D8B | XGetLanguage | 7 |
| 0x147DE1 | XGetVideoFlags | 8 (bits >> 16 & 0x5F: widescreen 0x01, 720p 0x02, 1080i 0x04, 480p 0x08, PAL60 0x40) |
| 0x147E2D | XGetAudioFlags | 9 (used by the sound object) |
| 0x147DB8 | XGetVideoStandard | 0x103 (AV region >> 8: 1 NTSC-M, 2 NTSC-J, 3 PAL) |
| 0x147DB0 | XGetAVPack | `HalBootSMCVideoMode` (kernel data export; 1 = HDTV) |

- **Language:** the static initialiser `0x18B950` sets the game language at **0x258818** from the table at 0x1CD6E0, indexed by XGetLanguage. Game index: 0 Japanese, 1 English (also used for "not set"), 2 French, 3 German, 4 Spanish, 5 Italian, matching the `Jpn/Eng/Frn/Ger/Spa/Ita` file suffixes (for example `0x28010` picks `A:\Media\Ending\tex_<lang>.dat`). About 90 game functions read 0x258818. **A launcher language option only needs the loader to answer setting 7.**
- **50/60 Hz:** the same initialiser writes a display mode at 0x25881C (0 = 60 Hz/HDTV, 1 = PAL60, 2 = PAL 50 Hz), but nothing else reads it. Device creation (`0x157510`) decides the refresh: **50 Hz when the video standard is PAL and the PAL60 flag is off, otherwise 60 Hz.** So the loader picks the mode through what it reports for settings 8 and 0x103: report PAL with PAL60 (or NTSC) for 60 Hz. Whether game speed or ADX timing then changes has to be checked at runtime.

## Z: cache (copy-on-read and background install)

- Cache manager: constructor `0x25CE0` (vtable 0x1C5A28, object 0x1920 bytes, created by `0x26520` from `0x12AB0`). It creates `Z:\Media` and a `Z:` copy of each content folder: Player, Enemy, People, Event, Event\Event, Event\TalkEvent, Misc, StgObj, Disp, Disp\charicon, Disp\map, Garage, Mission, Stage, Mark\DEFAULT and Cache. Each `D:\` path gets its drive letter swapped for `Z`.
- **Copy-on-read file loader:** `0x262E0` opens a `D:` file.
  - It first looks for an unfinished `Z:` copy (`name~`); if one is found, it deletes it and its target.
  - Then it opens the `Z:` copy if it exists, else the `D:` original.
  - `A:` paths (Title, Progress, Mark, Ending) are read from `D:` and never cached.
  - Files are opened with `0x60000000` (overlapped + no buffering) and read asynchronously into a buffer rounded up to the disk sector size.
  - `0x25910` polls the read with a 20 ms timeout. When the file came from `D:`, it writes the data to `Z:\...~` and then renames it to the real name.
  - `0x25820`/`0x258A0` run timed jobs from the tables at 0x1F1D08/0x1F1D90.
- **Background install:** `0x25550` is a state machine over **9 cache slots** (`D:\Media\Cache\Cache%02d.tbl` read by `0x267C0`, `DmCache%02d.tbl` by `0x268D0`). Each slot has a bitmap of copied files and a marker, `Z:\Media\Cache\JSRF_CACHE_COMPLETE%02d.CMP` (`0x25380` / `0x25400`). `JSRF_CACHE_COMPLETE.CMP` (`0x25460`, from `0x7C8C0`) marks the whole cache. `0x12740` (from `0x6F4D0`) writes `Z:\Media\Cache\JSRF_FATAL.ERR` on a fatal error.
- **Loader requirements** (ReWhacked lessons applied):
  - open host files with full sharing;
  - support overlapped `NtReadFile`/`NtWriteFile` with events (ReWhacked passes them to the host; check that completion reaches the game's event and APC);
  - **drop `FILE_NO_INTERMEDIATE_BUFFERING`** on host opens, or match Windows's alignment rules: the game's buffers are aligned for the Xbox, not necessarily for the host disk;
  - rename and delete on `Z:` (`name~` → `name`).
  - Unlike Whacked!, there is no file-time check on the cache.

## Camera

- **Matrix library** (MMATRIX section, 40 functions, 4,123 game calls): a Sega-style matrix stack.
  - The current matrix pointer is at 0x26B4C4, and the sine/cosine table at 0x26B464 (indexed by Sega angle >> 4).
  - Stack set-up is at `0x1BC150` (depth 16, from `0x5FA70`).
  - **`0x1BBA30` is LookAt(eye, target, roll)**, applied to the current matrix. It has 30 callers.
- Game code never calls the renderer's SetTransform: matrices from the stack reach the GPU as vertex-shader constants (`0x14E610`).
- **Camera classes:** 6 classes with 16-slot vtables at 0x1CB800, 0x1CB840, 0x1CB888, 0x1CB948, 0x1CB9D0 and 0x1CBA90 (constructors 0x50930, 0x50BE0, 0x51010, 0x514E0, 0x51720, 0x51C40). Each has an apply method (slot 3 or 6: `0x50AF0`, `0x50E50`, `0x511F0`, `0x514F0`, `0x518F0`, `0x51D70`). It runs when the draw pass (code 0x13) reaches it and:
  - calls SetRenderTarget (renderer slot 42) for the camera's target (+0xA4);
  - calls SetViewport (slot 30) with the rect at +0x84;
  - **calls SetProjection (slot 31) with the camera's angle at +0x74**, near 1.0, far 5000.0;
  - calls LookAt with the camera's eye/target data (+0x7C);
  - then draws its scene (+0x44).
- **For enhancements:**
  - Per-camera viewports are how split screen works.
  - +0x74 is where Hor+ widescreen and FOV options can act per camera (or globally on slot 31).
  - Modern camera control means changing eye/target before the LookAt in these apply methods.
  - Which class is the gameplay camera is still unknown. Check at runtime, for example by logging which apply methods run in a stage.

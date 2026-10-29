# Tests and the release gate

Before any build goes to testers, run:

```
python tools/release_check.py
```

`tools/make_release.py` runs it too, and won't package a build that fails it. The gate takes about four minutes and opens a game window for three of them.

## What it checks

1. **Unit tests** (`rejet_tests.exe`, built with the loader from the files here). Each one guards against a bug we've actually had, or one that would be expensive to miss:

| Group | Guards against |
|---|---|
| texture conversion | scrambled textures (the per-axis unswizzle tables must equal the Xbox swizzle, for every shape); dark seams around 2D art (colour bleed into transparent texels); DXT1 transparency detection; HD texture names changing (`sample_hash` pinned to release 0.2.1's values); missed texture updates (the change check sees a one-word change) |
| pixel shader constants | invisible graffiti: `SetPixelShader` loads the definition's own constants and `SetPixelShaderConstant` overwrites only mapped stages, as on the Xbox |
| widescreen | the projection widened twice (compounding); culling planes not widened (pop-in at the edges); 2D and other matrices touched |
| disc images | setup failing on trimmed or full disc images (synthetic images at each partition offset, nested directories); non-images half-read |
| settings | hand-edited or old `rejet.ini` values out of range; settings not loading back as saved |

2. **A scripted run** of about three minutes: the logos, the title, a new game, the opening cutscene, then skating in the Garage. It uses fixed settings (1080p 16:9 in a window) and fails on:
   - a crash, a fatal fault or a hang;
   - not reaching 3D gameplay;
   - a D3D function or render state ReJet ignores that isn't listed in `baseline_unimplemented.txt` (a new code path reached it: look at it before adding it to the list);
   - performance worse than this machine's baseline: CPU time per frame (median and worst 1%), and stutters a minute.

## The performance baseline

Performance only compares fairly on the same PC, so the baseline lives in `tests/local/perf_baseline.json`, which is git-ignored. Make one, or a new one after a change that is meant to cost time, with:

```
python tools/release_check.py --update-baseline
```

Run-to-run noise on one machine is large. On the development PC, the median CPU time per frame swings between about 3.8 and 4.9 ms on the Balanced power plan. So the gate allows 25% on the median and 30% on the worst 1%: it catches real regressions (3 ms more work per frame failed it), not small ones. For a precise comparison, record sessions with `REJET_PERFLOG=1` and compare them with `tools/perf_report.py` (docs/TESTING.md).

## Adding a test

- Only add a test that would catch a real regression. Pin the bug it guards against in a comment, with the date.
- Use synthetic inputs. Nothing derived from the game goes in the repository: no textures, shaders or data from the disc.
- Test the pure parts. If the code to test is mixed with Direct3D or Windows UI code, move the pure part into its own file first (as `texconv.c`, `widescreen.c` and `iso.c` were).
- Make sure the test fails without the fix: reintroduce the bug and run it once.

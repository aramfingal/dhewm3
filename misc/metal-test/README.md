# Metal renderer test harness

`metal_harness.py` runs real game sessions on a Mac, driven by scripted console configs, and
writes everything needed to judge a build into one folder: console output, screenshots,
side-by-side image diffs against the OpenGL renderer and against a baseline build, timedemo
numbers, crash reports and a `report.md` with a verdict. It is adapted from the ioq3 Metal
harness. The loop is: build, run the harness, read the report, fix, repeat.

Requirements: macOS, Python 3.9+, CMake + Ninja, `brew install sdl2-compat openal-soft`, and
the retail game data (`base/pak000.pk4`…, optionally `d3xp/`) in `~/Projects/doom-3-extracted`
or wherever `--data` points. The data is only read. numpy is optional (faster image diffs).

Runs stay in the background: the harness sets `SDL_MAC_BACKGROUND_APP=1`, and the game then
creates its window hidden and never becomes the active application. Screenshots are the same
as in a normal run. `--foreground` shows the window (it takes keyboard focus). Every launch
gets its own throwaway `fs_savepath`/`fs_configpath`, so your config and saves are untouched.

## Quick start

```sh
python3 misc/metal-test/metal_harness.py build --src .          # prints the binary
python3 misc/metal-test/metal_harness.py all --bin build        # Metal + OpenGL + diffs
python3 misc/metal-test/metal_harness.py all --bin build --baseline-bin ../dhewm3-base/build --validation
open metal-test-out/report.md
```

`--bin` accepts the binary, the `.app` or the build folder. Exit code: 0 for PASS/REVIEW,
1 for FAIL. `run --renderer opengl` runs one build with OpenGL only; `compare A B` diffs
two result folders (`--strict` treats flagged images as failures).

## What it runs

| scenario | what it checks |
|---|---|
| `menu` | the main menu draws (GUI path, cinematic background); two screenshots |
| `views_<game>_<map>` | six fixed views per map, compared across renderers and builds |
| `intro_base_mars_city1` | (full suite) the opening cinematic at four fixed tics |
| `restart_base_alphalabs1` | (full suite) `vid_restart` with a map loaded; screenshot before and after |
| `timedemo_demo1_rN` | `timeDemoQuit demo1`, fps (2 runs quick, 3 full) |
| `validation_<map>` | with `--validation`, Metal only: a views run under `MTL_DEBUG_LAYER=1` and `MTL_SHADER_VALIDATION=1` |

Quick suite maps: mars_city1, alphalabs1, hell1, d3xp erebus1. Full adds delta2a, d3xp hell.

How the views are made repeatable: the level loads with `com_fixedTic 1` (one game tic per
rendered frame), `skipCinematic` is issued ten times a second apart to get past any opening
cinematic, the level runs 150 more tics, then `god`, `notarget`, `noclip` and
`g_stopTime 1` freeze the world. The camera then visits six positions with `setviewpos`.
Positions come from the map file itself: item entities spread through the entity list, eye
56 units above each, yaw stepping by 90 degrees. Two OpenGL runs give images that differ in
at most about 0.3% of pixels; an image is flagged above 0.5%.

Timedemos run with `com_fixedTic -1`, which in this fork lets frames run without waiting for
the 60 Hz tic (stock dhewm3 holds a timedemo to 60 fps).

## Reading the results

```
metal-test-out/
  report.md            verdict, findings, per-scenario tables, image diff tables
  findings.json        the same findings, machine-readable
  diffs/*.png          triptychs: candidate | other | difference x4 in red (half size)
  candidate/<scenario>/
      result.json      status, reasons, suspicious console lines, metrics, image stats
      stdout.txt       process output (console text, Metal validation messages)
      harness.cfg      the console script that ran
      command.txt      the exact command line
      shots/*.png      screenshots (PNG for viewing, TGA for diffing)
      *.ips            macOS crash report, if the game crashed
  gl/…, baseline/…     same layout for the OpenGL run and the baseline build
```

Status per scenario: **FAIL** = crash, timeout, non-zero exit or a missing screenshot;
**WARN** = suspicious console lines or a blank (all black) screenshot; **PASS** otherwise.
Known-benign console noise is filtered (`BENIGN_PATTERNS`). Differences against OpenGL are
listed as REVIEW (two renderers are never pixel-identical); differences against a baseline
build of the same renderer are FAIL.

## Options

| option | effect |
|---|---|
| `--suite full` | all maps, intro and restart scenarios |
| `--only REGEX` | run only matching scenarios; other results in the folder are kept |
| `--res W H` | window size (default 1280 720) |
| `--no-timedemo` | skip the timedemo runs |
| `--no-gl` | (`all`) skip the OpenGL reference run |
| `--validation` | add the Metal API validation scenario |
| `--foreground` | show the game window |
| `--set NAME VALUE` | set a cvar in every launch (repeatable) |
| `--keep-home` | keep each launch's throwaway home folder |
| `--timeout N` | seconds per launch (default 300) |

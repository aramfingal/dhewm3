# dhewm3 with a Metal renderer

This fork (`aramfingal/dhewm3`) adds a native macOS Metal renderer to dhewm3, selectable at
startup. The OpenGL renderer stays the default and is the behavioural reference until Metal
matches it. Work happens on the `metal-renderer` branch.

## Rules

- Work on a feature branch (`metal-renderer`); commit early and often; push to `origin`
  (the fork). Never merge to `master` unless the owner asks.
- `origin` is `aramfingal/dhewm3`. `upstream` (`dhewm/dhewm3`) is for fetching only and its
  push URL is disabled (`git remote set-url --push upstream DISABLED-never-push-to-upstream`).
  Never open a pull request or issue against `dhewm/dhewm3`, and never push to it.
- Game data is in `~/Projects/doom-3-extracted` (`base/`, `d3xp/`). Treat it as read-only:
  point the game at it with `+set fs_basepath`, never modify it. Its `default.cfg` sets
  `sys_lang spanish`; the harness passes `+set sys_lang english`.
- Test runs must not take over the Mac. The harness sets `SDL_MAC_BACKGROUND_APP=1`, which
  makes the game create its window hidden and never become the active application (see
  `neo/sys/osx/SDLMain.m`, `neo/sys/glimp.cpp`). Launch the game by hand the same way.
- MSL is compiled at runtime, so a shader error does not fail the build: that shader just
  stops drawing. Compile changed MSL offline
  (`xcrun -sdk macosx metal -std=metal3.0 -c file.metal -o /dev/null`) and run the harness.
- Do not report anything as working unless the harness or a run confirmed it.
- Keep `PROGRESS.md` (done / verified / next) and `FOLLOWUPS.md` (known gaps) current.

## Build and test (macOS arm64)

Dependencies: `brew install cmake ninja sdl2-compat openal-soft` (plus Xcode for `metal`).

```sh
python3 misc/metal-test/metal_harness.py build --src .     # CMake + Ninja into build/, prints the binary
python3 misc/metal-test/metal_harness.py all --bin build   # Metal run + OpenGL run + image diffs
open metal-test-out/report.md
```

By hand: `cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
-DOPENAL_INCLUDE_DIR=/opt/homebrew/opt/openal-soft/include
-DOPENAL_LIBRARY=/opt/homebrew/opt/openal-soft/lib/libopenal.dylib ../neo && ninja` in `build/`.
The binary is `build/dhewm3.app/Contents/MacOS/dhewm3`; `base.dylib` and `d3xp.dylib` (game
code) are in `build/`. Run it with
`+set fs_basepath ~/Projects/doom-3-extracted +set fs_savepath <throwaway dir> +set fs_configpath <same>`.

`misc/metal-test/README.md` describes the scenarios, the output layout and the fix loop.

## Fork additions outside the renderer

- `skipCinematic` console command (`neo/framework/Session.cpp`): same as pressing escape
  during a cinematic. The harness uses it to reach a playable view.
- `com_fixedTic -1` no longer waits for the next 60 Hz tic at the end of a frame
  (`neo/framework/Common.cpp`), so `timeDemo` measures the renderer instead of the tic rate.
- `com_demoShotFrames "150 400 ..."`: while a render demo plays, writes
  `shots/demo_<frame>.tga` at exactly those demo frames (`neo/framework/Session.cpp`).
- `SDL_MAC_BACKGROUND_APP` handling (above).
- arm64 macOS deployment target raised from 10.7 to 11.0 (`neo/CMakeLists.txt`).

## Architecture

The Metal backend is selected with `+set r_backend metal` (default `opengl`; read when the
renderer starts). dhewm3 calls OpenGL only through `qgl*` function pointers filled by name
in `GLimp_ExtensionPointer` (`neo/sys/glimp.cpp`); with `r_backend metal` they point into
`neo/renderer/metal/`:

- `mgl.cpp`: the GL entry points dhewm3 uses, implemented on Metal (state, textures,
  buffers, draws, readback). `MGL_GetProcAddress` is the name lookup.
- `mgl_shader.cpp`: generates the MSL of a vertex/fragment pair; fixed-function stages.
- `mgl_arb.cpp`: translates ARB vertex/fragment program assembly to MSL.
- `metal_device.cpp`: device layer from the ioq3 Metal renderer (frame pacing, caches).
- `metal_host.cpp`: the only Metal file that includes engine headers (cvars, printing).
  The others are C++17 with metal-cpp (`neo/libs/metal-cpp`).

The renderer front end and back end (`draw_common.cpp`, `draw_arb2.cpp`, ...) are shared
and unchanged, so a rendering difference between the backends is a bug in
`neo/renderer/metal/`. Read `docs/metal/DESIGN.md` first; `GL_INVENTORY.md` lists the GL
call sites and `DEVICE_LAYER_CHANGES.md` the differences from the ioq3 device layer.

Debugging: `r_metalDebug 1` prints every shader pair compiled, `2` also writes its MSL to
`/tmp/mgl_pair_*.metal` (compile one with
`xcrun -sdk macosx metal -std=metal3.0 -c file.metal -o /dev/null`). A pair that fails to
compile is written to `/tmp/mgl_failed_*.metal` and draws nothing. `r_metalFrameStats N`
prints frame timing. Cvars: `r_metalHidden`, `r_metalNoPresent`, `r_metalPresentHz`.

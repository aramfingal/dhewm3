# Metal renderer progress

## Phase 0: baseline (done)

- Stock dhewm3 builds natively for arm64 (CMake + Ninja, sdl2-compat, openal-soft).
- Base game and d3xp start and render with the OpenGL renderer (verified by harness runs).
- Harness `misc/metal-test/metal_harness.py` runs against OpenGL: main menu, fixed views in
  mars_city1 / alphalabs1 / delta2a / hell1 / d3xp erebus1 / d3xp hell, the mars_city1
  intro, a `vid_restart` scenario, timedemo, console-problem and crash capture, image diffs,
  `report.md`.
- Verified: two OpenGL runs of the full suite compared against each other flag no image
  (largest difference 0.3% of pixels; frozen-world views differ by a few stray pixels).
- OpenGL baseline, M5 Max, 1280x720, hidden window: `timeDemo demo1` 152-166 fps
  (2148 frames). Reference screenshots (half size): `docs/metal/baseline-gl/`.
- Harness runs stay in the background (hidden window, app never activates): verified with
  `lsappinfo front` during a run.

## Phase 1: survey and seam (done)

- `docs/metal/GL_INVENTORY.md`: every OpenGL call site by file and function (1,103 calls,
  108 entry points outside the Windows tools).
- `docs/metal/DESIGN.md`: the seam is the existing `qgl*` function pointer table, filled by
  `GLimp_ExtensionPointer`. `r_backend metal` (read at start-up) makes `neo/sys/glimp.cpp`
  create an `SDL_WINDOW_METAL` window with `SDL_Metal_CreateView` and fill the table from the
  GL-on-Metal layer. OpenGL stays the default. Verified: OpenGL harness results unchanged.

## Phase 2: Metal backend (map parity reached; open items in FOLLOWUPS.md)

Done:
- `neo/renderer/metal/metal_device.cpp`: device layer derived from the ioq3 one
  (`docs/metal/DEVICE_LAYER_CHANGES.md`), metal-cpp in `neo/libs/metal-cpp`.
- `mgl_arb.cpp`: ARB vertex/fragment program assembly to MSL at load time. Every program
  the base game and d3xp load translates (interaction, ambient, environment, shadow, heat
  haze, soft particles, the d3xp post-process programs), including dhewm3's gamma patch.
- `mgl_shader.cpp`: fixed-function vertex and fragment stages (texture environment incl.
  combine, texgen, texture matrices, vertex colour, alpha test) as generated MSL.
- `mgl.cpp`: GL state, textures (2D, cube, 3D, S3TC/BPTC, sub-image, framebuffer and depth
  copies), buffer objects, client arrays, `glDrawElements`, immediate mode, two-sided
  stencil, polygon offset, scissored `glClear`, `glReadPixels`.

Verified with the harness on an M5 Max (1280x720, hidden window), commit of 2026-10-12:
- Full suite passes with Metal: main menu, mars_city1 intro, six maps (mars_city1,
  alphalabs1, delta2a, hell1, d3xp erebus1, d3xp hell), `vid_restart`, three timedemos.
- Metal against OpenGL: no image flagged (threshold 0.5% of pixels). Largest difference
  0.42% (alphalabs1 view5, the same view that varies most between two OpenGL runs).
  d3xp erebus1 varies by about 3/255 from run to run in either renderer's harness run
  (wall-clock dependent content, see FOLLOWUPS).
- `demoshots_demo1` (16 frames of `timeDemo demo1`, exact scenes; two OpenGL runs are
  bit-identical): Metal differs from OpenGL in at most 0.001% of pixels per frame (mean
  difference at most 0.03 of 255). This scenario found two real bugs, both fixed: vertex
  colours and the texture environment colour were not clamped to [0,1] (glow materials).
- Metal API validation and shader validation (`MTL_DEBUG_LAYER=1`,
  `MTL_SHADER_VALIDATION=1`) on alphalabs1: no messages.
- `timeDemo demo1`: Metal 652-683 fps, OpenGL 204 fps. Metal does not present in a hidden
  window and OpenGL swaps a hidden window, so this compares the renderers, not presentation.
- Presenting (`r_metalHidden 0`, window still hidden by the harness): menu, alphalabs1 and
  the timedemo run and present (about 118 fps on a 120 Hz display).
- `r_gamma 1.6`, `r_brightness 1.4`: Metal matches OpenGL as closely as with the defaults.

- Dear ImGui (the dhewm3 settings menu, `dhewm3Settings`): the `settings_menu` scenario
  draws it over the main menu; Metal and OpenGL screenshots match.
- `r_multiSamples 4`: Metal renders into multisampled targets and resolves; against
  OpenGL at 4x the demo frames differ in at most 0.06% of pixels. Validation clean at 4x.
- Mirrors, heat-haze glass and cube-map reflections (`special_*` scenarios, positions taken
  from the map data): Metal matches OpenGL (at most 0.1% of pixels).
- `set r_backend opengl; vid_restart` and back to `metal` in one session
  (`switch_base_alphalabs1`): works both ways; the first and third screenshots are identical.
- Debug views `r_showTris 2`, `r_showLights 2`, `r_showNormals 2`, `r_showShadows 1`,
  `r_showLightCount 1`, `r_showOverDraw 1` on alphalabs1: Metal matches OpenGL except for
  line rasterisation in `r_showTris` (1.4% of pixels).
- CPU cost in the timedemo (`r_metalFrameStats`): 1.4-1.8 ms per frame in total, of which
  0.4-0.5 ms from the first GL call of a frame to the end of present; no time waiting for
  the GPU.
- The OpenGL renderer is unchanged: the current build with `r_backend opengl` against a
  build of the Phase 0 commit (worktree `../dhewm3-base`) flags no image and has the same
  timedemo result (189.2 fps both).

Not verified:
- A visible window (focus, resize, fullscreen, presentation pacing on screen): every run so
  far kept the window hidden so the Mac stayed usable.
- Remote camera GUIs, the d3xp post-process materials (blood orb, enviro suit) and the
  debug tools (`r_show*`) go through the same GL layer but no harness scenario looks at them.

## Frame rate independent of the game tic (branch `uncapped-fps`, on top of `metal-renderer`)

Design and cvars: `docs/FRAMERATE.md`. The game still runs 60 tics per second; frames are
drawn as fast as possible and show entities, lights, skeletons and the view interpolated
between the last two tics, with mouse look applied in every frame.

Verified with the harness (M5 Max, 1280x720, hidden window, 2026-10-13):
- Frame rate in maps with `com_smoothFrames 1`, no limit: Metal 1,900-3,400 fps, OpenGL
  330-470 fps (alphalabs1, the mars_city1 intro, d3xp erebus1). Before: 60.
- `smooth_base_alphalabs1` (both renderers, three runs each): a frame half way between two
  tics equals a frame taken from half way; alpha 0 equals the tic before; a view turned 10
  degrees by pending input equals really facing there, first person weapon included.
- `smooth_intro_base_mars_city1`: during the opening cinematic an alpha 0 frame reproduces
  the frame of the tic before (differences only in GUI screens, which run per tic).
- Camera path in the intro at 240 fps (`r_interpolateLog`): the camera advances every frame
  in proportion to the time passed, across tic boundaries; 2,569 moving frames, no backward
  step, one outlier (a camera cut).
- In play, every frame interpolates entities (up to 49), lights (up to 7) and the skeletons
  in view (counts from `r_interpolateLog`).
- `com_maxFPS 240` gives 4.0 frames per tic.
- The whole earlier suite still passes with both renderers, and with `wait` counting frames
  the frozen views became repeatable: no image differs between Metal and OpenGL in more
  than 0.05% of pixels.

Not verified (needs a person at the screen): how it feels, i.e. mouse look with direct look
on and off, vsync at the display's refresh rate, a long play session. Multiplayer is
unchanged and still tied to tics.

## Armor mounted flashlight (`g_mountedFlashlight`, branch `uncapped-fps`)

The flashlight key switches a light on the player's armor instead of swapping the weapon
(BFG Edition style; the Duct Tape mod only covers two weapons and replaces weapon defs the
HD pack also replaces). Game code only, base and RoE; `toggleFlashlight` command for a key
of its own. Harness scenario `flashlight_*` (off, on, off again): passes on OpenGL and Metal
in alphalabs1 and erebus1, and the two renderers' images match.

Not verified: by a person in play.

Kept in savegames and across levels (player spawnArgs, format unchanged). The scenario saves
with the light on, turns it off and loads: the loaded view is brighter (fails without the
change). Passes in alphalabs1 and erebus1 on Metal.

## Soft particles with multisampling (Metal)

A reported dip to 150 fps (alphalabs1, steam, 2560x1440, 8x MSAA) was the depth copy that
soft particles need: it ended the multisampled scene pass. The depth buffer is now drawn
again into a single-sample target instead (docs/metal/DESIGN.md, "Depth copies with
multisampling"). Measured from the owner's save, hidden window, not presenting: 152 -> 338
fps at that spot, 527 -> 1026 fps with no particles in view; OpenGL gives 114 there. Harness
at 8x MSAA (views, special, settings) passes with and without the replay and the two match.

## Steam and multisampling, second round (Metal)

Same save (alphalabs1, steam, 2560x1440, 8x MSAA, hidden window, not presenting):

| | fps |
|---|---|
| before any of this | 152 |
| depth copy by replay | 338 |
| last pass of a frame does not store samples, depth, stencil | 410 |
| soft particles in a half-size layer (`r_metalParticleLayer 2`, the default) | 1,166 |
| same view without particles | 2,217 (was 1,026) |

Verified: harness views, special, settings, switch, demoshots, flashlight on Metal with the
layer on and off, with 0 and 8 samples, all pass and no image is flagged between on and off;
Metal against OpenGL at the harness defaults passes. Tried and dropped: depth testing soft
particles one radius nearer (same image, no gain: the hidden steam is within its radius of a
surface) and discarding faded fragments (slower).

Not verified: how the layer looks in play (a person at the screen), particle-heavy fights.

Depth-aware upsampling of the layer (DESIGN.md): at half size, steam no longer bleeds over
nearer edges. Same save, full-size layer against half-size in the same run: 3,338 pixels
off by more than 4 levels (max 56) with bilinear upsampling, 48 (max 10) now. 1,172 ->
1,120 fps there.

## Shader compiles at start-up (Metal)

Pairs compiled by earlier runs are listed in `fs_savepath/metal_shaders.cache` and compiled
at start-up on all cores (DESIGN.md, "Shader compiles"). With every compile forced cold
(steam save, 8x MSAA): the level's first frames stalled 1,171 + 161 ms before, none now; the
10 recorded pairs took 97 ms at start-up. Warm: under 1 ms. Harness full suite on Metal
passes.

## Heat haze with multisampling (Metal)

After the main view's `_currentRender` copy the frame is finished without multisampling
(`r_metalPostProcessSingleSample`, DESIGN.md). GPU time per frame (`r_metalFrameStats`, new
`gpu` column), 2560x1440, 8x MSAA, three alternating runs while other apps were busy:

| view (harness `special_*` heat haze) | before | after |
|---|---|---|
| hell1 | 10.6-11.0 ms | 6.2 ms |
| mars_city1 | 6.3-6.4 ms | 4.55 ms |

Screenshots of the two show no difference apart from animation and the fps counter.

## Vsync at 240 Hz (Metal, `r_metalEarlyCommit`)

At 2560x1440, 8x, HD pack, Metal draws 1,000-1,850 fps uncapped in the airlock at the start of
commoutside (OpenGL 177-327) and 1,650-2,250 outdoors in mc_underground (-656 -2112 96). But
with vsync on the frame was committed only after `nextDrawable` returned: the GPU idled while
the CPU waited for the display, and its time per frame doubled, more than a 240 Hz refresh
(4.17 ms). Now the frame is committed before the wait and a second command buffer copies it
to the drawable. Presented, vsync on, GPU ms per frame:

| view | `r_metalEarlyCommit 0` | `1` |
|---|---|---|
| commoutside airlock | 5.4-6.0 | 2.45-2.66 |
| mc_underground outdoors | 5.3-6.4 | 2.95-3.67 |

The hidden test window is shown at 120 Hz whatever the frame costs, so the effect at 240 Hz
is not verified (FOLLOWUPS 5).
Info.plist now has `LSApplicationCategoryType` (games), so macOS can use Game Mode.

## Next

- FOLLOWUPS.md: a visible-window test, a harness scenario for subviews and post-process
  materials, per-draw CPU cost, shader pair compilation at start-up.

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
  d3xp erebus1 shows a uniform offset of about 3/255 over the whole image (see FOLLOWUPS).
- Metal API validation and shader validation (`MTL_DEBUG_LAYER=1`,
  `MTL_SHADER_VALIDATION=1`) on alphalabs1: no messages.
- `timeDemo demo1`: Metal 652-683 fps, OpenGL 204 fps. Metal does not present in a hidden
  window and OpenGL swaps a hidden window, so this compares the renderers, not presentation.
- Presenting (`r_metalHidden 0`, window still hidden by the harness): menu, alphalabs1 and
  the timedemo run and present (about 118 fps on a 120 Hz display).
- `r_gamma 1.6`, `r_brightness 1.4`: Metal matches OpenGL as closely as with the defaults.

Not verified:
- A visible window (focus, resize, fullscreen, presentation pacing on screen): every run so
  far kept the window hidden so the Mac stayed usable.
- Mirrors and remote cameras, the d3xp post-process materials and debug tools (`r_show*`)
  are implemented through the same GL layer but no harness scenario looks at them yet.

## Next

- FOLLOWUPS.md, in order: Dear ImGui under Metal, a harness scenario for subviews and
  post-process materials, the erebus1 offset, MSAA, per-draw CPU cost.

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

## Next

- Phase 1: inventory of OpenGL call sites, renderer backend seam, `r_backend` cvar.
- Phase 2: Metal backend.

# Known gaps and follow-ups

Each item: where, approach, risk, how to verify.

## Harness

1. **Frozen-world views only.** `views_*` scenarios stop game time (`g_stopTime 1`) before
   moving the camera, so particles, animated lights and moving models are compared only in
   the `intro_*` scenario and the timedemo (no image compare there). Approach: record a
   `recordDemo` per map and take shots at fixed demo frames. Verify: two GL runs must match.
2. **The engine log (`dhewm3log.txt`) is written to `~/Library/Application Support/dhewm3`**
   even with a throwaway `fs_savepath`: the path is fixed before cvars are read
   (`neo/sys/osx/DOOMController.mm`). Harmless, but not isolated.
3. **The frozen views are occasionally frozen at a different game time.** In one of about
   ten runs `special_base_hell1/heathaze` showed the lava and flames at another animation
   phase (3% of pixels) in one renderer; a rerun matched. `g_stopTime` is set a fixed number
   of rendered frames after the map starts, and frames do not always map to game tics the
   same way (also the cause of item 4, probably). Approach: stop time from the game side at
   a fixed `gameLocal.time`, or use render demos (`recordDemo`) for these views as
   `demoshots_demo1` does. Verify: ten runs of one renderer are identical.
3a. **Intro shots are not exactly repeatable** (up to about 0.1% of pixels between two GL
   runs): the cinematic is running while the shot is taken.

## Metal backend

4. **d3xp erebus1 views are not repeatable** (harness, not renderer): the whole frame is
   about 3/255 darker in some runs. Three Metal runs gave mean brightness 73.31, 76.31 and
   73.31; OpenGL gave 76.31 every time, and a Metal run that lands on 76.31 is bit-identical
   to OpenGL. Something in that level depends on wall-clock time (probably a GUI overlay,
   which uses real time, not game time). Approach: find it with `g_showHud 0` and by
   disabling GUIs, or drop the level from the image comparison; `demoshots_demo1` is the
   exact comparison. Verify: three runs of one renderer match.
5. **A visible window has not been tested** (see PROGRESS.md). Check focus, input grab,
   `r_fullscreen`, resizing (`MGL_Resize` recreates the scene target) and `r_swapInterval 1`.
   With vsync off, `r_metalPresentHz -1` should keep the frame rate above the refresh rate;
   in the hidden-window test every frame still waited about 5 ms in `nextDrawable`.
6. **Per-draw CPU cost.** Every draw rebuilds its uniform block and vertex layout
   (`PrepareDraw` in `mgl.cpp`). Ideas: skip the uniform upload when the block is unchanged,
   cache the layout per array state, copy client-memory index arrays only once per surface
   (dhewm3 has `r_useIndexBuffers`), prewarm pipelines. Measure with `r_metalFrameStats`.
   Verify: screenshots unchanged, backend ms lower.
7. **Shader pairs are compiled on first use** (a hitch of some milliseconds each, about 40
   pairs in a level). The OS caches them between runs. Approach: compile the pairs seen in
   earlier runs at start-up, or asynchronously as in the ioq3 layer.
8. **Not implemented in the GL layer** (logged once as "not implemented" when called):
   `glGetTexImage` / `glGetCompressedTexImage` (writing precompressed images), `glDrawPixels`
   and `glRasterPos` (some `r_show*` tools), colour-index textures, line width (always 1),
   eye-linear texgen is implemented but untested, `GL_EXT_depth_bounds_test` (not exposed; dhewm3
   then skips it).
9. **Compressed internal formats with uncompressed data** (`image_useCompression`) are
   stored as RGBA8: Metal cannot compress at upload. More memory than OpenGL in that mode.
10. **No harness scenario for** remote camera GUIs, the d3xp post-process materials
    (blood orb, enviro suit, `motionBlur.vfp`) or the `r_show*` debug views. The programs
    translate; whether they look right is unverified. Approach: a d3xp view with the
    artifact active (needs `give` and `impulse` commands in the script), and one view per
    `r_show*` cvar compared against OpenGL.

## Frame rate (`docs/FRAMERATE.md`)

11. **Never played by a person.** Everything was verified with scripted runs in a hidden
    window. Check mouse feel (`com_smoothFramesDirectLook 0/1`), `r_swapInterval 1` on a
    120 Hz display, weapon and flashlight when turning fast, GUIs, cinematics, saving and
    loading, d3xp (grabber, mounted guns, slow motion artifact).
12. **HUD and in-world GUIs change at 60 Hz.** They are drawn every frame but with the game
    time of the latest tic (`hud->Redraw( gameLocal.realClientTime )` in the game code).
    Approach: pass an interpolated time from the game's draw code. Risk: GUI scripts that
    compare times. The GUI cursor on in-world screens therefore trails a directly turned
    view by up to a tic.
13. **View kick, bob and shake under direct look** are interpolated as "what the game adds
    to the input" (`InterpView`), which has only been checked for not disturbing the
    existing tests: no scripted test fires a weapon. Check by eye that recoil is smooth and
    the view does not wobble at the pitch limits.
14. **Not interpolated:** shader parms (light colour flicker, beam end points), entity
    bounds, the sound listener. A beam's start is interpolated and its end is not.
15. **Lights that move are re-derived every frame** (`R_InterpSetLightTransform`), as they
    were once per tic before. With many moving lights at very high frame rates this is the
    largest cost of interpolation (alphalabs1: 3,250 fps without skeleton interpolation,
    2,780 with; erebus1 2,100 and 1,900).
16. **Multiplayer is still one frame per tic**: the client's prediction loop
    (`neo/framework/async/AsyncClient.cpp`) would need the same treatment.
17. **Upstream's `com_gameHz` branch** (`upstream/kickstart-my-hertz`, 34 commits) would
    lower the one tic of display latency by running the game faster; it changes physics.
    It can be merged on top of this: the interpolation code does not assume 60 Hz except
    in `wait` and `com_preciseFrameLengthMS`.

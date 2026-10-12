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
3. **Intro shots are not exactly repeatable** (up to about 0.1% of pixels between two GL
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

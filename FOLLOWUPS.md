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

4. **Dear ImGui (the dhewm3 settings menu) does not run with Metal.** `neo/sys/glimp.cpp`
   skips `D3::ImGuiHooks::Init` when `usingMetal`, and the hooks in `neo/sys/sys_imgui.cpp`
   return early. Approach: the ImGui GL2 backend already draws through `qgl`; it needs
   `glPushAttrib`/`glPopAttrib` (now no-ops in `mgl.cpp`), `glGetIntegerv` for the state it
   saves, 16 bit indices (done) and scissor in window coordinates. Or use ImGui's Metal
   backend (Objective-C++). Risk: low. Verify: open the menu (F10) in a visible window.
5. **d3xp erebus1: uniform offset of about 3/255 against OpenGL** over the whole frame
   (`metal-test-out/diffs/gl__views_d3xp_erebus1__view*.png`, mean diff 3.0, max 4 away from
   the weapon). Probably one full-screen blended pass (fog or a post-process material) that
   rounds differently. Approach: `r_skipPostProcess`, `r_skipFogLights`, `r_skipBlendLights`
   one at a time in both renderers to find the pass, then compare its blend and texture
   format. Risk: low. Verify: the erebus1 views drop to the level of the other maps.
6. **No MSAA.** `r_multiSamples` is ignored with Metal (the scene target is single sample).
   Approach: multisample colour and depth attachments in `GLMetal_EnsureSceneTargets` with a
   resolve before copies, readback and present; sample count in the pipeline key. Risk:
   medium (every blit from the scene needs the resolved texture). Verify: harness with
   `--set r_multiSamples 4` against OpenGL.
7. **A visible window has not been tested** (see PROGRESS.md). Check focus, input grab,
   `r_fullscreen`, resizing (`MGL_Resize` recreates the scene target) and `r_swapInterval 1`.
   With vsync off, `r_metalPresentHz -1` should keep the frame rate above the refresh rate;
   in the hidden-window test every frame still waited about 5 ms in `nextDrawable`.
8. **Per-draw CPU cost.** Every draw rebuilds its uniform block and vertex layout
   (`PrepareDraw` in `mgl.cpp`). Ideas: skip the uniform upload when the block is unchanged,
   cache the layout per array state, copy client-memory index arrays only once per surface
   (dhewm3 has `r_useIndexBuffers`), prewarm pipelines. Measure with `r_metalFrameStats`.
   Verify: screenshots unchanged, backend ms lower.
9. **Shader pairs are compiled on first use** (a hitch of some milliseconds each, about 40
   pairs in a level). The OS caches them between runs. Approach: compile the pairs seen in
   earlier runs at start-up, or asynchronously as in the ioq3 layer.
10. **Not implemented in the GL layer** (logged once as "not implemented" when called):
   `glGetTexImage` / `glGetCompressedTexImage` (writing precompressed images), `glDrawPixels`
   and `glRasterPos` (some `r_show*` tools), colour-index textures, eye-linear texgen is
   implemented but untested, line width, `GL_EXT_depth_bounds_test` (not exposed; dhewm3
   then skips it).
11. **Compressed internal formats with uncompressed data** (`image_useCompression`) are
   stored as RGBA8: Metal cannot compress at upload. More memory than OpenGL in that mode.
12. **No harness scenario for subviews** (mirrors, remote camera GUIs), heat haze and the
    d3xp post-process programs, or `r_show*` debug views. Approach: find fixed positions in
    front of a mirror (mars_city1 bathroom) and a heat-haze emitter and add a `views` list
    with explicit coordinates to `metal_harness.py`.

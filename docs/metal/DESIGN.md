# Metal renderer design

## The seam

dhewm3 never calls OpenGL directly. Every call goes through a `qgl*` function pointer
(`neo/renderer/qgl.h`, `qgl_proc.h`), and every pointer is filled by name through one function,
`GLimp_ExtensionPointer` (`neo/sys/glimp.cpp`), at start-up (`R_InitOpenGL`,
`R_CheckPortableExtensions`). Window and context creation, buffer swap and swap interval are
also in `glimp.cpp`.

That table is the renderer backend seam. `r_backend` (`opengl`, the default, or `metal`; read
when the renderer starts: `+set r_backend metal` on the command line, or change it and
`vid_restart`, which recreates the window) selects what fills it:

| | `r_backend opengl` | `r_backend metal` |
|---|---|---|
| window | `SDL_WINDOW_OPENGL` + `SDL_GL_CreateContext` | `SDL_WINDOW_METAL` + `SDL_Metal_CreateView` |
| `qgl*` pointers | `SDL_GL_GetProcAddress` | `MGL_GetProcAddress` (`neo/renderer/metal/mgl.cpp`) |
| swap | `SDL_GL_SwapWindow` | `MGL_SwapBuffers` (present the Metal drawable) |

Everything above the table (front end, `draw_common.cpp`, `draw_arb2.cpp`, image loading, the
vertex cache, GUIs, debug tools) is shared by both backends and unchanged. The whole diff
outside `neo/renderer/metal/` is a few dozen lines in `glimp.cpp`, the cvar, and the build.

### Why this seam and not a second back end

The alternative was a separate Metal back end behind `RB_ExecuteBackEndCommands`, with its own
versions of the depth fill, interaction, shader pass, fog and shadow code. It was rejected for
now:

- Doom 3's back end is written against the fixed-function GL state machine (texture
  environment, texgen, matrix stacks, client arrays) mixed with ARB assembly programs. A second
  back end would duplicate about 4,000 lines of draw logic and then have to be kept identical
  to the GL one by hand. With the table seam both backends run the same draw logic, so they
  agree by construction and the harness diffs measure the Metal layer only.
- Every path works at once: debug tools (`r_show*`, immediate mode), Dear ImGui (its GL2
  backend already goes through `qgl`), mods' own `glprogs`, cinematics, subviews.
- The stated goal is the smallest workable diff with GL as the reference.

The cost is a state-tracking layer between the engine and Metal, and no Metal-specific
restructuring of the frame (one render pass per frame, no parallel encoding). Hot paths can be
moved to direct Metal code later, behind the same cvar, once parity is established; see
`FOLLOWUPS.md`.

## Layers

```
neo/renderer/*.cpp (shared)         qglXxx(...)
        |
neo/renderer/metal/mgl.cpp          the GL entry points dhewm3 uses, implemented on Metal:
        |                           state tracking, draw calls, textures, buffers, readback
neo/renderer/metal/mgl_ff.cpp       fixed-function vertex/fragment stages as generated MSL
neo/renderer/metal/mgl_arb.cpp      ARB vertex/fragment program assembly -> MSL translator
        |
neo/renderer/metal/metal_device.cpp device layer from the ioq3 Metal renderer (GLMetal_*):
                                    drawable and frame pacing, frame arenas, pipeline and
                                    depth/stencil caches, samplers, uploads, MSL compilation
neo/libs/metal-cpp                  Apple's C++ Metal headers
```

The Metal files do not include idlib or the renderer headers (they are C++17, the engine is
C++11): `metal_host.cpp` is the bridge for printing and cvars.

## Conventions

- **Image orientation.** The scene is rendered upside down into an off-screen target: every
  vertex shader ends with `y = -y`, and the front-face winding is clockwise. Row `r` of the
  target is then GL window row `r`, so viewport, scissor, `glReadPixels`, `glCopyTexImage2D`
  and `fragment.position` use GL coordinates unchanged. The final pass to the drawable flips
  the image.
- **Depth.** GL clip space z is in [-w, w]; every vertex shader ends with `z = (z + w) / 2`.
  `glDepthRange` maps to the Metal viewport's near/far.
- **Programs.** A pipeline's shader pair is generated from two keys: the vertex stage (fixed
  function configuration, or an ARB vertex program) and the fragment stage (fixed function
  configuration, or an ARB fragment program), compiled together on first use and cached.
  Uniforms are one block of `float4` slots per pair, holding only what that pair reads.
- **Position invariance.** The fixed-function vertex stage and ARB programs with
  `OPTION ARB_position_invariant` compute the position with the same expression, marked
  `[[invariant]]`, so the depth-equal passes match the depth fill.
- **Gamma and brightness.** dhewm3 applies `r_gamma`/`r_brightness` by patching every ARB
  fragment program (`r_gammaInShader`). The translator sees the patched text, so Metal matches.

## Depth copies with multisampling

Soft particles (`r_useSoftParticles`, on by default) copy the depth buffer to a texture
right after the depth fill. A copy means ending the scene pass and opening it again, and
with multisampling that stores and reloads every sample; blending onto reloaded samples is
then about three times slower than onto samples that never left the tile. In a steam-filled
corridor of alphalabs1 at 2560x1440 with 8 samples this was 6.6 ms a frame.

So with multisampling the device layer records the depth-writing draws of a frame
(`GLMetal_RecordDepthDraw`), and a depth copy draws them again into a single-sample depth
target in a command buffer of its own, committed at once so it runs before the frame's. The
scene pass stays open. Because that buffer runs first, each depth copy of a frame goes to a
storage of its own (`CopySceneDepth` in `mgl.cpp`), or a second view's copy would be what the
first view's particles read. The copied depth is that of pixel centres instead of sample 0;
images differ in about 0.003% of pixels. `r_metalNoDepthReplay 1` gives the old path.
Colour copies (heat haze) still end the pass.

## The end of a multisampled frame

The scene pass leaves its store actions open and chooses them when it ends
(`GLMetal_EndEncoderIfNeeded`). A pass that ends in the middle of a frame stores everything,
because the next pass loads it. The last pass of a frame only resolves colour and drops the
samples, depth and stencil. Storing them was more than half the cost of an 8-sample frame at
2560x1440 (1,026 fps before, 2,217 after, in a view without particles).

## Presenting (`r_metalEarlyCommit`)

A presented frame is two command buffers: the frame, committed as soon as it is encoded, and
a copy of the scene target into the drawable, committed after `nextDrawable` returns (it blocks
until the display hands a drawable back). Commit order on the one queue keeps them in order;
the in-flight semaphore is signalled by the second. With one command buffer committed after
the wait (`r_metalEarlyCommit 0`), the GPU idled while the CPU waited and macOS lowered its
clock: with vsync on, GPU time per frame doubled.

## Heat haze with multisampling (`r_metalPostProcessSingleSample`)

Post-process surfaces (heat haze, glass distortion) read `_currentRender`, a copy of the
scene made right before the first of them. A copy ends the scene pass; with multisampling,
opening it again loads every sample of colour, depth and stencil, and everything blended
after that is slow (the same cost as depth copies had). After the main view's copy only
post-process surfaces and 2D are drawn (every other sort comes first, fog lights included).
So the back end calls `GLimp_EndMultisamplingAtNextCopy` before that copy when the view is
not a subview, and the device layer ends the pass with resolve-only store actions for colour,
depth and stencil (`GLMetal_EndEncoderIfNeeded( false, true )`): the samples go into the
single-sample scene textures, which the rest of the frame draws into. Pipelines made then have
one sample (`PIPELINE_SINGLE_SAMPLE` in the cache key), the particle layer is off, and the
frame ends with a plain store.

What is lost: antialiased edges of the post-process surfaces themselves (their masks are soft)
and of anything 3D drawn after the main view. Copies in subviews keep the old way.
`r_metalPostProcessSingleSample 0` turns it off.

## Soft particles in a layer (`r_metalParticleLayer`)

Steam and smoke are stacks of large blended quads, and filling them into every sample of the
scene target is what a view full of steam costs. Soft particles are not depth tested and have
no hard edges, so they need neither the samples nor the resolution.

The shared back end brackets a soft particle draw with `GLimp_BeginParticleLayer` /
`GLimp_EndParticleLayer` (no-ops on OpenGL). The Metal device layer then records the draw
instead of making it (`GLMetal_BeginBlendLayer`). Before anything else is drawn into the
scene, or the pass ends, the recorded draws go into a single-sample RGBA16F target of
1/scale the size, in a command buffer committed at once (it runs before the frame's, like
the depth replay, whose depth texture it reads). The target starts as (0,0,0,1): rgb collects
colour with the particle's own blend, alpha collects how much of the background is left.
One rectangle in the scene pass then blends it over the scene (`colour + scene * alpha`).

Smaller than the scene, the layer is upsampled with the scene depth (`layer_depth_fs`), the
last depth copy of the frame, which is what the particles faded against. Each layer texel
stands for the scene pixel at the centre of its block; of the four texels around a pixel
only those whose scene depth is within 10% of the pixel's (as distance: depth is
`1 - near / z`, so `1 - d` ratios are distance ratios) are blended, and if none is, the
closest one is used. Without this, steam behind a nearer edge (a pipe, the weapon) bled a
pixel or two onto it. Away from depth edges the result is the plain bilinear sample.

- Only blends that add to the background or cover it by source alpha qualify (source
  `ONE`/`SRC_ALPHA`, destination `ONE`/`ONE_MINUS_SRC_ALPHA`); anything else is drawn as before.
- Order is kept: any other draw into the scene ends the group collected so far. A frame can
  have 4 groups; further particles are drawn directly.
- The back end multiplies `PP_CURDEPTH_RECIPR` by the scale, because `fragment.position` is in
  the layer's pixels.
- It needs the depth copy of the frame to have been made by replay, so the replay is used
  without multisampling as well.
- `0` turns it off (what the harness compares with OpenGL), `1` is a full-size layer, `2`
  (default) half, `4` quarter size. The image differs slightly from `0`: about one 8-bit step
  in dense steam (the layer blends in floating point, the scene target rounds after every
  quad). At half size against the full-size layer, at the owner's steam save (2560x1440):
  48 pixels differ by more than 4 levels, at most 10 (3,338 and 56 with plain bilinear
  upsampling); it costs about 4% of the frame rate there.

## Shadow maps (`r_shadowMaps`)

Stencil shadow volumes cost what their volumes cover on screen, times the samples per pixel:
next to a building outdoors in mc_underground, at 8x, they were four fifths of the frame. With
`r_shadowMaps 1` (off by default, Metal only) a light's shadow casters are drawn into depth
maps seen from the light, and its interactions are multiplied by how much light the maps let
through. The code is `renderer/draw_shadowmap.cpp` (back end, shared) and the shadow parts of
`metal_device.cpp`, `mgl.cpp` and `mgl_shader.cpp`.

- **Casters.** The shadow volumes the front end makes anyway: their caps are the casters'
  triangles that face away from the light, and `shadow.vp` sends the extruded vertexes to
  infinity, the far plane seen from the light. A map therefore holds the nearest back face
  of a caster, so lit faces do not shadow themselves and no depth bias is needed (the bias
  cvars default to 0). dmap's precomputed volumes of the world (`r_useOptimizedShadows`) do
  not have such caps: with shadow maps the world makes ordinary volumes instead
  (`R_UseOptimizedShadows`), and changing `r_shadowMaps` makes all interactions again
  (`R_ShadowMapsFrame`, as `regenerateWorld` does). Those volumes get their caster's bounds,
  which culls them per map layer.
- **Projections.** A light whose origin is inside its volume (point lights) gets six 90 degree
  layers, one per axis direction; any other (projected lights; parallel lights are point
  lights 100000 units away) one layer just wide enough for the volume. A light whose volume
  reaches behind its origin keeps stencil shadows. `r_shadowMapSize` is the size for a light
  that covers the screen, halved or quartered for smaller ones and halved per point light
  layer.
- **Self shadowing.** As with stencil shadows, surfaces with a noselfshadow material are
  shadowed by the other casters only: they get a map without their group's casters
  (`localShadows`).
- **Drawing.** Each map is a `Depth32Float` 2D array texture, drawn layer by layer in a
  command buffer of its own that is committed before the frame's
  (`GLMetal_CommitShadowWork`), so the scene pass is never interrupted. While a layer is
  drawn it is the current encoder: the scene's encoder and its cached state are put aside
  (`GLMetal_ShadowMapBeginLayer`), pipelines get a depth-only variant
  (`PIPELINE_SHADOW_TARGET`), and the GL layer sets the viewport and scissor to the map,
  restoring the back end's afterwards (`MGL_ShadowMapBeginLayer`). Maps are pooled by size
  and reused from frame to frame; Metal's hazard tracking orders a reuse.
- **Lookup.** A pair drawn while `MGL_ShadowSample` is on gets `MglPairKey::shadow`: its
  vertex function passes the eye space position, and its fragment function multiplies the
  colour by `mgl_shadow()` (eye to world, layer by major axis, 3x3 comparison samples with
  bilinear filtering, `r_shadowMapSoftness` texels apart). A surface facing away from the
  light (flat normal from the position's derivatives) is in shadow, as stencil volumes have
  it whatever the normal map says; not a noselfshadow one, which its own caps do not shadow. The parameters are `MU_SHADOW` uniforms; the map is
  texture 15.

Against stencil shadows (the harness with `--set r_shadowMaps 1`): edges are soft, and the
view weapon gets the shadow at its own position instead of the camera's. `r_shadowMapDebug`
1-4 prints each light's setup or shows the light let through, the map coordinates, the map
depth.

## Shader compiles

A shader pair is MSL generated for one vertex/fragment combination, compiled with
`newLibrary`. With the source in the system's Metal shader cache that takes well under a
millisecond; without it (the first run after the generator or macOS changed, or after the
system dropped its cache) 40-170 ms a pair, and loading a save stalled 1.2 s on first use.

So `mgl.cpp` appends the key of every pair that compiles to `metal_shaders.cache` in
`fs_savepath`, and before the first pair is looked up compiles all pairs in that file at once,
on all cores (`dispatch_apply_f`, `GLMetal_CompileLibraryAnyThread`). A key names its ARB
programs by GL id and a hash of their translated text instead of their version, which
depends on load order; records whose programs are gone or changed are skipped, and
duplicates are dropped when the file is read. The layer composite shader is compiled when
the device starts. `r_metalDebug 1` prints, for each frame that compiled anything, how many
shaders and pipelines it made and how long they took.

Measured with every compile cold (alphalabs1 save, 8x MSAA): 10 recorded pairs compiled at
start-up in 97 ms; the first frames of the level made no compiles (1,171 + 161 ms before).
Warm: under 1 ms for all of them.

## What the GL layer implements

The inventory (`GL_INVENTORY.md`) lists 108 entry points. Implemented groups: state
(`glEnable`, blend, depth, stencil incl. `glStencilOpSeparate`, cull, scissor, polygon offset
and mode, colour mask, alpha test), matrices, client arrays and `glVertexAttribPointerARB`,
`glDrawElements`, immediate mode, textures (2D, cube, 3D; uncompressed and S3TC/BPTC; sub-image;
copies from the framebuffer and depth buffer), texture environment and texgen, ARB vertex
buffer objects, ARB programs, `glClear`, `glReadPixels`. Entry points dhewm3 never calls on
macOS are bound to a stub that logs once.

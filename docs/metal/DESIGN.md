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

## What the GL layer implements

The inventory (`GL_INVENTORY.md`) lists 108 entry points. Implemented groups: state
(`glEnable`, blend, depth, stencil incl. `glStencilOpSeparate`, cull, scissor, polygon offset
and mode, colour mask, alpha test), matrices, client arrays and `glVertexAttribPointerARB`,
`glDrawElements`, immediate mode, textures (2D, cube, 3D; uncompressed and S3TC/BPTC; sub-image;
copies from the framebuffer and depth buffer), texture environment and texgen, ARB vertex
buffer objects, ARB programs, `glClear`, `glReadPixels`. Entry points dhewm3 never calls on
macOS are bound to a stub that logs once.

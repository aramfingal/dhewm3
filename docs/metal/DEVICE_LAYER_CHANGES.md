# Device layer: changes from the ioq3 original

`neo/renderer/metal/metal_device.cpp` is derived from `code/renderermetal/tr_metal_device.cpp`
of aramfingal/ioq3 (5,504 lines there, about 1,000 here). This file records what was kept,
changed, dropped and added, so the two can later be merged into one shared library.

The ioq3 layer serves a renderer that already speaks in Quake 3 terms (GL state bits,
texnums, VAOs, a fixed set of post-process passes). dhewm3's Metal backend sits under a
general OpenGL layer instead, so the parts keyed on Quake 3 state had to become general,
and the Quake 3 passes had no use here.

## Kept (same code, reformatted to dhewm3 style)

| ioq3 | here | notes |
|---|---|---|
| frame arenas: `MetalFrameArena`, `GLMetal_ArenaAlloc`, `GLMetal_FrameAlloc`, `GLMetal_ResetFrameArena`, `GLMetal_ReleaseFrameArenas` | same names | unchanged |
| `GLMetal_SetSharedUniforms` | same | the inline-bytes fallback was dropped (it returned early in practice) |
| encoder state cache `MetalEncoderStateCache`, `GLMetal_OnEncoderOpened`, `GLMetal_SetRenderPipelineState`, `GLMetal_SetVertexBuffer`, `GLMetal_SetFragmentBuffer` | same | cache extended (see Changed) |
| in-flight frame semaphore: `GLMetal_AcquireInFlightFrameSlot`, `GLMetal_ReleaseInFlightFrameSlot`, completion handler in `GLMetal_Present` | same | unchanged |
| frame stats (`r_metalFrameStats`), `GLMetal_DisplayRefreshRate`, `GLMetal_NowNs` | same | cvar read through `MetalHost_CvarInt` |
| present pacing rules (`r_metalNoPresent`, `r_metalPresentHz`, `r_metalHidden`) | `GLMetal_ShouldPresent` | same rules, split out of `GLMetal_AcquireDrawableForPresent` |
| `GLMetal_UpdateDisplaySyncSetting` | same | no `modificationCount`: compares the value |
| upload batching: `GLMetal_FlushUploads`, `GLMetal_GetUploadBlitEncoder`, staging buffer per upload | same | see Changed for ordering |
| `GLMetal_LoadShaderLibraryFromSource` (with `setPreserveInvariance`) | same | the `WithDefines`, pair and asynchronous variants were not brought over |
| shutdown order (drain the queue before releasing anything) | `GLMetal_Shutdown` | unchanged |
| `GLMetal_HashCombine`, `MetalErrorString` | same | unchanged |

## Changed

| what | ioq3 | here | why |
|---|---|---|---|
| host interface | `ri.Printf`, `ri.Cvar_Get`, `cvar_t`, `qboolean`, `q_shared.h` | `metal_host.h` (`MetalHost_Printf`, `MetalHost_Warning`, `MetalHost_CvarInt`), plain `bool` | the layer no longer includes any engine header; a shared library would take this interface |
| render targets | any texnum pair, drawable as a target, per-target pending clears | one scene target (BGRA8 + Depth32Float_Stencil8) owned by the layer; `GLMetal_Present` draws it into the drawable | OpenGL has one framebuffer; the scene is stored upside down (GL row order), the present pass flips it |
| drawable acquisition | at the first pass that targets the drawable | only in `GLMetal_Present` | nothing renders into the drawable directly, so no frame ever waits for a drawable before its work is encoded |
| off-screen frames | separate `s_offscreenTarget` | not needed: a frame that is not presented simply skips the present pass | |
| front-face winding | counter-clockwise | clockwise | consequence of the upside-down scene |
| viewport / scissor | one GL rectangle, flipped here, scissor = viewport | `GLMetal_SetViewport`, `GLMetal_SetScissor`: separate rectangles in target rows, no flip | GL scissor is independent of the viewport |
| depth/stencil state | key built from Q3 `GLS_*` bits plus packed stencil bits, one stencil face | `MetalDepthStencilKey`: compare function, write flag, separate front/back stencil ops, masks | two-sided stencil for shadow volumes, any compare function |
| cull mode | 0/1/2 with Q3 meaning | `MTL::CullMode` | |
| pipelines | key: program name string, Q3 state bits, vertex descriptor hash, attachment formats; registry by name + VAO hash; async prewarm | key: program id, blend key (factors, enable, write mask), vertex layout hash; `MetalVertexLayout` describes attributes and buffers | blend factors and colour masks are arbitrary GL state; one attachment format; no prewarm yet |
| samplers | key: wrap mode, linear, mipmap, mip-linear; global anisotropy | `MetalSamplerKey`: min/mag/mip filter, three address modes, anisotropy, max lod, lod bias, border colour | OpenGL sampler state is per texture |
| textures | table indexed by texnum, 2D only, created and uploaded in one call | the layer creates (`GLMetal_NewTexture`: 2D, cube, 3D, mip count, swizzle) and uploads regions (`GLMetal_UploadTextureRegion`); the GL layer owns the name table | GL defines textures level by level, needs cube and 3D targets and luminance/alpha formats (swizzle) |
| upload ordering | always in the batch command buffer committed before the frame | in the batch buffer while the frame has no work; once it has, in the frame's own command buffer | a texture can be redefined between two draws of one frame (cinematics, scratch images) |
| readback | capture of the presented drawable, one frame late, on request | `GLMetal_ReadScenePixels`: blit to a buffer, commit, wait; colour, depth or stencil | `glReadPixels` is synchronous and reads the current frame |
| fragment texture binding | binding table by texnum resolved in the layer | `GLMetal_SetFragmentTexture( texture, sampler, slot )`, filtered | the GL layer resolves names |

## Dropped (Quake 3 specific)

Shadow volume and shadow finish draws, tonemap, greyscale, shadow mask, depth blur and SAO
passes, `GLMetal_RenderBlitTexture*`, `GLMetal_FastCopyTexture`, depth-only render targets,
depth clamp, the pipeline self test, ray tracing hooks (`tr_metal_rt.h`), static buffers
(`GLMetal_CreateStaticBuffer`: the GL layer makes buffer objects itself), the white texture.

## Added

- `GLMetal_FlushAndWait`: commit what is encoded, wait, continue the frame in a new command buffer.
- `GLMetal_CommandBufferForBlit`: the frame's command buffer with the pass ended, for ordered blits.
- `GLMetal_CopySceneToTexture`: colour copy, or depth copy through a buffer into an R32Float texture.
- `GLMetal_EncoderGeneration`: lets the caller notice that a new encoder has lost its state.
- `GLMetal_SetFillMode`, `GLMetal_ReleaseTexture`, `GLMetal_ReleaseBuffer` (they also clear the bind filter).
- `GLMetal_Resize`.
- Multisampling: `GLMetal_DeviceInit( ..., samples )` renders into multisampled colour and
  depth/stencil textures and resolves into the single-sample scene textures every time the
  pass ends (`StoreActionStoreAndMultisampleResolve`), so copies, readback and present are
  unchanged. The sample count is part of every pipeline.

## For a shared library

The host-neutral core would be: device/queue/layer, frame lifecycle and pacing, arenas, the
encoder state cache, the three caches with the general keys used here, uploads, readback and
MSL compilation, behind `metal_host.h`. ioq3's render-target table, binding table and
post-process passes would sit on top of it as a Quake 3 module.

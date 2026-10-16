# OpenGL call inventory

Generated from the source by counting `qgl*` and `SDL_GL_*` calls.
The Windows-only tools (`neo/tools`, 821 calls) are not listed: they are not built on macOS.

## By file

| file | calls | what it does with GL |
|---|---|---|
| `neo/renderer/tr_rendertools.cpp` | 345 | debug views (r_show*), immediate mode, readback of colour/depth/stencil |
| `neo/renderer/draw_common.cpp` | 283 | depth fill, material stage passes (fixed function texenv/texgen and ARB programs), stencil shadows, fog and blend lights, post-process |
| `neo/renderer/tr_backend.cpp` | 114 | GL state cache (GL_State, GL_Cull, GL_TexEnv), default state, buffer swap, back end command loop |
| `neo/renderer/Image_load.cpp` | 102 | texture creation and upload (2D, cube, 3D, compressed), filter and wrap, framebuffer and depth copies, purge |
| `neo/renderer/tr_render.cpp` | 69 | draw calls (glDrawElements), vertex and texcoord pointers, matrices, scissor, depth hacks |
| `neo/renderer/draw_arb2.cpp` | 66 | interaction pass with ARB vertex/fragment programs, program loading, ambient lights |
| `neo/sys/glimp.cpp` | 41 | window, GL context, swap, swap interval, function lookup (SDL) |
| `neo/renderer/RenderSystem_init.cpp` | 22 | start-up: function pointers, extension checks, screenshots (glReadPixels), gfxInfo |
| `neo/renderer/VertexCache.cpp` | 15 | ARB vertex buffer objects |
| `neo/renderer/tr_trace.cpp` | 13 | debug trace drawing |
| `neo/renderer/Image_init.cpp` | 10 | texture filter changes, binding helpers |
| `neo/sys/sys_imgui.cpp` | 8 | Dear ImGui hooks: saves and restores GL state around the ImGui pass |
| `neo/renderer/MegaTexture.cpp` | 5 | megatexture updates (unused by the game data) |
| `neo/renderer/RenderWorld_portals.cpp` | 5 | debug portal drawing |
| `neo/renderer/Image.h` | 2 | inline bind helpers |
| `neo/renderer/RenderSystem.cpp` | 2 | GL error check, viewport for cropped renders |
| `neo/renderer/GuiModel.cpp` | 1 | - |

## By function

### `neo/renderer/GuiModel.cpp`

| function | calls | GL entry points |
|---|---|---|
| `idGuiModel::EmitFullScreen` | 1 | glOrtho |

### `neo/renderer/Image.h`

| function | calls | GL entry points |
|---|---|---|
| `Code` | 2 | glEnable, glDisable |

### `neo/renderer/Image_init.cpp`

| function | calls | GL entry points |
|---|---|---|
| `R_BorderClampImage` | 1 | glTexParameterfv |
| `idImageManager::ChangeTextureFilter` | 4 | glTexParameterf |
| `idImageManager::SetNormalPalette` | 2 | glColorTableEXT, glEnable |
| `idImageManager::BindNull` | 3 | glDisable |

### `neo/renderer/Image_load.cpp`

| function | calls | GL entry points |
|---|---|---|
| `idImage::UploadCompressedNormalMap` | 1 | glTexImage2D |
| `idImage::SetImageFilterAndRepeat` | 15 | glTexParameterf |
| `idImage::GenerateImage` | 3 | glTexImage2D, glGenTextures |
| `idImage::Generate3DImage` | 17 | glTexParameterf, glTexImage3D, glGenTextures |
| `idImage::GenerateCubeImage` | 11 | glTexParameterf, glTexParameteri, glTexImage2D, glGenTextures |
| `idImage::WritePrecompressedImage` | 3 | glPixelStorei, glGetCompressedTexImageARB, glGetTexImage |
| `idImage::UploadPrecompressedImage` | 4 | glGenTextures, glCompressedTexImage2DARB, glTexImage2D, glTexParameteri |
| `idImage::PurgeImage` | 1 | glDeleteTextures |
| `idImage::Bind` | 10 | glDisable, glEnable, glBindTexture, glPrioritizeTextures |
| `idImage::BindFragment` | 4 | glBindTexture |
| `idImage::CopyFramebuffer` | 11 | glCopyTexSubImage2D, glTexParameterf, glReadBuffer, glCopyTexImage2D, glTexImage2D |
| `idImage::CopyDepthbuffer` | 8 | glTexParameterf, glCopyTexSubImage2D, glReadBuffer, glTexImage2D |
| `idImage::UploadScratch` | 14 | glTexParameterf, glTexImage2D, glTexSubImage2D, glTexParameteri |

### `neo/renderer/MegaTexture.cpp`

| function | calls | GL entry points |
|---|---|---|
| `idMegaTexture::BindForViewOrigin` | 4 | glProgramLocalParameter4fvARB |
| `idTextureLevel::UpdateTile` | 1 | glTexSubImage2D |

### `neo/renderer/RenderSystem.cpp`

| function | calls | GL entry points |
|---|---|---|
| `idRenderSystemLocal::CaptureRenderToFile` | 2 | glReadBuffer, glReadPixels |

### `neo/renderer/RenderSystem_init.cpp`

| function | calls | GL entry points |
|---|---|---|
| `R_CheckPortableExtensions` | 6 | glGetIntegerv, glGetFloatv, glDebugMessageCallbackARB, glEnable |
| `R_InitOpenGL` | 5 | glGetString, glGetIntegerv |
| `GL_CheckErrors` | 1 | glGetError |
| `R_RenderingFPS` | 2 | glFinish |
| `R_ReadTiledPixels` | 5 | glReadPixels, glReadBuffer, glGetIntegerv |
| `R_StencilShot` | 1 | glReadPixels |
| `R_VidRestart_f` | 1 | glGetError |
| `idRenderSystemLocal::InitOpenGL` | 1 | glGetError |

### `neo/renderer/RenderWorld_portals.cpp`

| function | calls | GL entry points |
|---|---|---|
| `idRenderWorldLocal::ShowPortals` | 5 | glColor3f, glBegin, glVertex3fv, glEnd |

### `neo/renderer/VertexCache.cpp`

| function | calls | GL entry points |
|---|---|---|
| `idVertexCache::ActuallyFree` | 2 | glBindBufferARB, glBufferDataARB |
| `idVertexCache::Position` | 2 | glBindBufferARB |
| `idVertexCache::UnbindIndex` | 1 | glBindBufferARB |
| `idVertexCache::Alloc` | 6 | glBufferDataARB, glBindBufferARB, glGenBuffersARB |
| `idVertexCache::AllocFrameTemp` | 2 | glBindBufferARB, glBufferSubDataARB |
| `idVertexCache::EndFrame` | 2 | glBindBufferARB |

### `neo/renderer/draw_arb2.cpp`

| function | calls | GL entry points |
|---|---|---|
| `GL_SelectTextureNoClient` | 1 | glActiveTextureARB |
| `RB_ARB2_DrawInteraction` | 23 | glProgramEnvParameter4fvARB |
| `RB_ARB2_CreateDrawInteractions` | 24 | glBindProgramARB, glEnableVertexAttribArrayARB, glVertexAttribPointerARB, glDisableVertexAttribArrayARB, glEnable, glDisable, glEnableClientState, glColorPointer, glVertexPointer, glDisableClientState |
| `RB_ARB2_DrawInteractions` | 12 | glStencilFunc, glEnable, glBindProgramARB, glDisableClientState, glScissor, glClear, glDisable, glEnableClientState |
| `R_LoadARBProgram` | 6 | glGetError, glBindProgramARB, glProgramStringARB, glGetIntegerv, glGetString |

### `neo/renderer/draw_common.cpp`

| function | calls | GL entry points |
|---|---|---|
| `RB_PrepareStageTexturing` | 51 | glEnable, glTexGenfv, glBindProgramARB, glNormalPointer, glEnableClientState, glTexGenf, glTexCoordPointer, glVertexAttribPointerARB, glEnableVertexAttribArrayARB, glMatrixMode, glPolygonOffset, glLoadMatrixf |
| `RB_FinishStageTexturing` | 31 | glDisable, glMatrixMode, glTexGenf, glDisableVertexAttribArrayARB, glDisableClientState, glLoadIdentity, glTexCoordPointer, glBindProgramARB |
| `RB_T_FillDepthBuffer` | 11 | glEnable, glColor4fv, glDisable, glTexGenfv, glPolygonOffset, glVertexPointer, glTexCoordPointer, glAlphaFunc |
| `RB_STD_FillDepthBuffer` | 8 | glEnable, glDisableClientState, glTexCoord2f, glEnableClientState, glPolygonOffset, glStencilFunc, glDisable |
| `RB_SetProgramEnvironment` | 7 | glProgramEnvParameter4fvARB |
| `RB_SetProgramEnvironmentSpace` | 4 | glProgramEnvParameter4fvARB |
| `RB_STD_T_RenderShaderPasses` | 64 | glTexEnvi, glEnable, glBindProgramARB, glDisable, glEnableClientState, glDisableClientState, glProgramEnvParameter4fvARB, glColorPointer, glVertexAttribPointerARB, glEnableVertexAttribArrayARB, glDisableVertexAttribArrayARB, glColor4fv, glLoadMatrixf, glScissor, glPolygonOffset, glVertexPointer, glTexCoordPointer, glNormalPointer, glProgramLocalParameter4fvARB, glTexEnvfv |
| `RB_STD_DrawShaderPasses` | 2 | glEnableClientState, glColor3f |
| `RB_T_Shadow` | 30 | glStencilOp, glStencilOpSeparate, glColor3f, glProgramEnvParameter4fvARB, glVertexPointer, glDepthBoundsEXT, glDisable, glEnable |
| `RB_StencilShadowPass` | 10 | glEnable, glStencilFunc, glDisable, glDisableClientState, glPolygonOffset, glEnableClientState, glStencilOp |
| `RB_T_BlendLight` | 6 | glTexGenfv, glVertexPointer |
| `RB_BlendLight` | 15 | glEnable, glDisable, glDisableClientState, glMatrixMode, glTexCoord2f, glColor4fv, glLoadIdentity |
| `RB_T_BasicFog` | 4 | glTexGenfv |
| `RB_FogPass` | 13 | glEnable, glDisable, glDisableClientState, glTexCoord2f, glColor3fv |
| `RB_STD_FogAllLights` | 8 | glDisable, glEnable, glScissor, glClear, glStencilFunc, glStencilOp |
| `RB_STD_LightScale` | 18 | glVertex2f, glLoadIdentity, glMatrixMode, glDisable, glScissor, glPushMatrix, glOrtho, glColor3f, glBegin, glEnd, glPopMatrix, glEnable |
| `RB_STD_DrawView` | 1 | glStencilFunc |

### `neo/renderer/tr_backend.cpp`

| function | calls | GL entry points |
|---|---|---|
| `RB_SetDefaultGLState` | 26 | glDisable, glEnable, glTexGenf, glEnableClientState, glClearDepth, glColor4f, glDisableClientState, glColorMask, glPolygonMode, glDepthMask, glDepthFunc, glCullFace, glShadeModel, glScissor |
| `GL_SelectTexture` | 2 | glActiveTextureARB, glClientActiveTextureARB |
| `GL_Cull` | 6 | glCullFace, glDisable, glEnable |
| `GL_TexEnv` | 1 | glTexEnvi |
| `GL_State` | 16 | glDepthFunc, glEnable, glAlphaFunc, glDepthMask, glPolygonMode, glBlendFunc, glColorMask, glDisable |
| `RB_SetGL2D` | 9 | glMatrixMode, glLoadIdentity, glDisable, glViewport, glScissor, glOrtho |
| `RB_SetBuffer` | 6 | glClearColor, glDrawBuffer, glClear |
| `RB_ShowImages` | 14 | glTexCoord2f, glVertex2f, glFinish, glClearColor, glClear, glBegin, glEnd |
| `RB_SwapBuffers` | 33 | glDisable, glVertex2f, glIsEnabled, glEnable, glMatrixMode, glBlendEquation, glPushMatrix, glLoadIdentity, glColor4f, glPopMatrix, glBlendFunc, glOrtho, glBegin, glEnd, glFinish |
| `RB_ExecuteBackEndCommands` | 1 | glBindTexture |

### `neo/renderer/tr_render.cpp`

| function | calls | GL entry points |
|---|---|---|
| `RB_DrawElementsImmediate` | 4 | glBegin, glTexCoord2fv, glVertex3fv, glEnd |
| `RB_DrawElementsWithCounters` | 2 | glDrawElements |
| `RB_DrawShadowElementsWithCounters` | 2 | glDrawElements |
| `RB_RenderTriangleSurface` | 2 | glVertexPointer, glTexCoordPointer |
| `RB_EnterWeaponDepthHack` | 4 | glMatrixMode, glDepthRange, glLoadMatrixf |
| `RB_EnterModelDepthHack` | 4 | glMatrixMode, glDepthRange, glLoadMatrixf |
| `RB_LeaveDepthHack` | 4 | glMatrixMode, glDepthRange, glLoadMatrixf |
| `RB_RenderDrawSurfListWithFunction` | 2 | glLoadMatrixf, glScissor |
| `RB_RenderDrawSurfChainWithFunction` | 2 | glLoadMatrixf, glScissor |
| `RB_LoadShaderTextureMatrix` | 3 | glMatrixMode, glLoadMatrixf |
| `RB_BindStageTexture` | 13 | glEnable, glTexGenf, glTexCoordPointer, glMatrixMode, glEnableClientState, glNormalPointer, glLoadMatrixf |
| `RB_FinishStageTexture` | 14 | glMatrixMode, glDisable, glTexGenf, glLoadIdentity, glTexCoordPointer, glDisableClientState |
| `RB_BeginDrawingView` | 11 | glMatrixMode, glDisable, glLoadMatrixf, glViewport, glScissor, glStencilMask, glClearStencil, glClear, glEnable |
| `RB_CreateSingleDrawInteractions` | 2 | glLoadMatrixf, glScissor |

### `neo/renderer/tr_rendertools.cpp`

| function | calls | GL entry points |
|---|---|---|
| `RB_DrawBounds` | 22 | glVertex3f, glBegin, glEnd |
| `RB_SimpleSurfaceSetup` | 2 | glLoadMatrixf, glScissor |
| `RB_SimpleWorldSetup` | 2 | glLoadMatrixf, glScissor |
| `RB_PolygonClear` | 15 | glDisable, glVertex3f, glPushMatrix, glPushAttrib, glLoadIdentity, glBegin, glEnd, glPopAttrib, glPopMatrix |
| `RB_ShowDestinationAlpha` | 1 | glColor3f |
| `RB_ScanStencilBuffer` | 1 | glReadPixels |
| `RB_CountStencilBuffer` | 1 | glReadPixels |
| `R_ColorByStencilBuffer` | 7 | glStencilFunc, glClearColor, glDisable, glClear, glStencilOp, glColor3fv |
| `RB_ShowIntensity` | 11 | glLoadIdentity, glMatrixMode, glReadPixels, glPushMatrix, glOrtho, glRasterPos2f, glPopMatrix, glColor3f, glDrawPixels |
| `RB_ShowDepthBuffer` | 28 | glTexCoord2f, glVertex2f, glPopMatrix, glMatrixMode, glPushMatrix, glLoadIdentity, glOrtho, glRasterPos2f, glColor3f, glEnable, glColor4f, glBegin, glEnd, glReadPixels, glDrawPixels |
| `RB_ShowLightCount` | 7 | glStencilOp, glClearStencil, glClear, glEnable, glStencilFunc, glVertexPointer |
| `RB_ShowSilhouette` | 15 | glArrayElement, glDisable, glColor3f, glDisableClientState, glVertexPointer, glBegin, glEnd, glEnable |
| `RB_ShowShadowCount` | 6 | glClearStencil, glClear, glEnable, glStencilOp, glStencilFunc, glVertexPointer |
| `RB_T_RenderTriangleSurfaceAsLines` | 4 | glVertex3fv, glBegin, glEnd |
| `RB_ShowTris` | 11 | glDisable, glEnable, glDisableClientState, glColor3f, glPolygonOffset, glDepthRange |
| `RB_ShowSurfaceInfo` | 9 | glDisable, glEnable, glDisableClientState, glColor3f, glPolygonOffset, glDepthRange |
| `RB_ShowViewEntitys` | 12 | glDisable, glColor3f, glDisableClientState, glLoadMatrixf, glEnable, glDepthRange |
| `RB_ShowTexturePolarity` | 11 | glColor4f, glVertex3fv, glDisableClientState, glDisable, glColor3f, glBegin, glEnd |
| `RB_ShowUnsmoothedTangents` | 8 | glVertex3fv, glDisableClientState, glDisable, glColor4f, glBegin, glEnd |
| `RB_ShowTangentSpace` | 8 | glColor4f, glDisableClientState, glDisable, glBegin, glVertex3fv, glEnd |
| `RB_ShowVertexColor` | 6 | glDisableClientState, glDisable, glBegin, glColor4ubv, glVertex3fv, glEnd |
| `RB_ShowNormals` | 16 | glVertex3fv, glColor3f, glDisable, glEnable, glDisableClientState, glBegin, glEnd |
| `RB_AltShowNormals` | 19 | glVertex3fv, glColor3f, glDisable, glEnable, glDisableClientState, glBegin, glEnd |
| `RB_ShowTextureVectors` | 9 | glVertex3fv, glColor3f, glDisableClientState, glBegin, glEnd |
| `RB_ShowDominantTris` | 9 | glVertex3fv, glDisableClientState, glPolygonOffset, glEnable, glColor3f, glBegin, glEnd, glDisable |
| `RB_ShowEdges` | 13 | glVertex3fv, glColor3f, glBegin, glEnd, glDisableClientState, glDisable, glEnable |
| `RB_ShowLights` | 10 | glDisable, glEnable, glDisableClientState, glColor4f, glColor3f, glDepthRange |
| `RB_ShowPortals` | 2 | glDisable, glEnable |
| `RB_DrawText` | 5 | glVertex3fv, glBegin, glColor3fv, glEnd |
| `RB_ShowDebugText` | 4 | glLineWidth, glDisable, glEnable |
| `RB_ShowDebugLines` | 14 | glVertex3fv, glLineWidth, glBegin, glEnd, glDisable, glColor3fv, glEnable, glColor4fv |
| `RB_ShowDebugPolygons` | 14 | glDisable, glEnable, glPolygonOffset, glColor4fv, glBegin, glVertex3fv, glEnd, glDepthRange |
| `RB_TestGamma` | 12 | glLoadIdentity, glMatrixMode, glColor3f, glPushMatrix, glDisable, glOrtho, glRasterPos2f, glDrawPixels, glPopMatrix, glEnable |
| `RB_TestGammaBias` | 12 | glLoadIdentity, glMatrixMode, glColor3f, glPushMatrix, glDisable, glOrtho, glRasterPos2f, glDrawPixels, glPopMatrix, glEnable |
| `RB_TestImage` | 18 | glTexCoord2f, glVertex2f, glLoadIdentity, glMatrixMode, glColor3f, glPushMatrix, glOrtho, glBegin, glEnd, glPopMatrix |
| `RB_RenderDebugTools` | 1 | glScissor |

### `neo/renderer/tr_trace.cpp`

| function | calls | GL entry points |
|---|---|---|
| `RB_DrawExpandedTriangles` | 7 | glVertex3f, glBegin, glEnd |
| `RB_ShowTrace` | 6 | glColor4f, glDisableClientState, glLoadMatrixf |

### `neo/sys/glimp.cpp`

| function | calls | GL entry points |
|---|---|---|
| `GLimp_Init` | 25 | SDL_GL_SetAttribute, SDL_GL_GetAttribute, SDL_GL_GetProcAddress, SDL_GL_CreateContext |
| `GLimp_GetCurState` | 8 | SDL_GL_GetAttribute |
| `GLimp_Shutdown` | 1 | SDL_GL_DeleteContext |
| `GLimp_SwapBuffers` | 2 | SDL_GL_SwapWindow, SDL_GL_SwapBuffers |
| `GLimp_ExtensionPointer` | 1 | SDL_GL_GetProcAddress |
| `GLimp_SetSwapInterval` | 3 | SDL_GL_SetSwapInterval |
| `GLimp_UpdateWindowSize` | 1 | SDL_GL_GetDrawableSize |

### `neo/sys/sys_imgui.cpp`

| function | calls | GL entry points |
|---|---|---|
| `EndFrame` | 8 | glDisable, glBindBufferARB, glGetIntegerv |

## Entry points used, by count

`glDisable` 118, `glEnable` 91, `glTexParameterf` 55, `glVertex3fv` 43, `glMatrixMode` 42, `glColor3f` 39, `glProgramEnvParameter4fvARB` 39, `glDisableClientState` 31, `glBegin` 28, `glEnd` 28, `glVertex3f` 25, `glLoadIdentity` 22, `glVertex2f` 20, `glTexEnvi` 19, `glBindProgramARB` 18, `glTexGenfv` 18, `SDL_GL_GetAttribute` 18, `glTexGenf` 16, `glTexCoord2f` 16, `glColor4f` 16, `glEnableClientState` 15, `glLoadMatrixf` 15, `glStencilOp` 15, `glScissor` 13, `glStencilFunc` 12, `glPopMatrix` 12, `SDL_GL_SetAttribute` 12, `glBindBufferARB` 11, `glTexImage2D` 10, `glVertexPointer` 10, `glPolygonOffset` 10, `glPushMatrix` 10, `glOrtho` 9, `glTexCoordPointer` 9, `glStencilOpSeparate` 9, `glBindTexture` 8, `glReadPixels` 8, `glEnableVertexAttribArrayARB` 8, `glVertexAttribPointerARB` 8, `glDisableVertexAttribArrayARB` 8, `glClear` 8, `glDepthRange` 8, `glGetIntegerv` 7, `glColor4fv` 7, `glCopyTexSubImage2D` 6, `glClearColor` 6, `glTexParameteri` 5, `glReadBuffer` 5, `glProgramLocalParameter4fvARB` 5, `glGetString` 5, `glGetError` 5, `glFinish` 5, `glNormalPointer` 5, `glCullFace` 5, `glGenTextures` 4, `glBufferDataARB` 4, `glColorPointer` 4, `glAlphaFunc` 4, `glColor3fv` 4, `glDepthFunc` 4, `glDrawElements` 4, `glRasterPos2f` 4, `glDrawPixels` 4, `glArrayElement` 4, `glLineWidth` 4, `glTexSubImage2D` 3, `glPolygonMode` 3, `glDepthMask` 3, `glIsEnabled` 3, `glClearStencil` 3, `SDL_GL_GetProcAddress` 3, `SDL_GL_SetSwapInterval` 3, `glTexImage3D` 2, `glActiveTextureARB` 2, `glColorMask` 2, `glBlendFunc` 2, `glViewport` 2, `glBlendEquation` 2, `glTexParameterfv` 1, `glColorTableEXT` 1, `glPixelStorei` 1, `glGetCompressedTexImageARB` 1, `glGetTexImage` 1, `glCompressedTexImage2DARB` 1, `glDeleteTextures` 1, `glPrioritizeTextures` 1, `glCopyTexImage2D` 1, `glGetFloatv` 1, `glDebugMessageCallbackARB` 1, `glGenBuffersARB` 1, `glBufferSubDataARB` 1, `glProgramStringARB` 1, `glTexEnvfv` 1, `glDepthBoundsEXT` 1, `glClearDepth` 1, `glShadeModel` 1, `glClientActiveTextureARB` 1, `glDrawBuffer` 1, `glTexCoord2fv` 1, `glStencilMask` 1, `glPushAttrib` 1, `glPopAttrib` 1, `glColor4ubv` 1, `SDL_GL_CreateContext` 1, `SDL_GL_DeleteContext` 1, `SDL_GL_SwapWindow` 1, `SDL_GL_SwapBuffers` 1, `SDL_GL_GetDrawableSize` 1
